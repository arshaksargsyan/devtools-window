/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include <string.h>
#include "jsapi.h"
#include "jscntxt.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsprvtd.h"
#include "jsnum.h"
#include "jsproxy.h"

#include "gc/Marking.h"
#include "vm/Shape.h"

#include "jsatominlines.h"
#include "jsinferinlines.h"
#include "jsobjinlines.h"

#include "vm/RegExpObject-inl.h"

using namespace js;
using namespace js::gc;

static inline HeapSlot &
GetCall(RawObject proxy)
{
    JS_ASSERT(IsFunctionProxy(proxy));
    return proxy->getSlotRef(JSSLOT_PROXY_CALL);
}

static inline Value
GetConstruct(RawObject proxy)
{
    if (proxy->slotSpan() <= JSSLOT_PROXY_CONSTRUCT)
        return UndefinedValue();
    return proxy->getSlot(JSSLOT_PROXY_CONSTRUCT);
}

static inline HeapSlot &
GetFunctionProxyConstruct(RawObject proxy)
{
    JS_ASSERT(IsFunctionProxy(proxy));
    JS_ASSERT(proxy->slotSpan() > JSSLOT_PROXY_CONSTRUCT);
    return proxy->getSlotRef(JSSLOT_PROXY_CONSTRUCT);
}

void
js::AutoEnterPolicy::reportError(JSContext *cx, jsid id)
{
    if (JSID_IS_VOID(id)) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_OBJECT_ACCESS_DENIED);
    } else {
        JSString *str = IdToString(cx, id);
        const jschar *prop = str ? str->getCharsZ(cx) : NULL;
        JS_ReportErrorNumberUC(cx, js_GetErrorMessage, NULL,
                               JSMSG_PROPERTY_ACCESS_DENIED, prop);
    }
}

#ifdef DEBUG
void
js::AutoEnterPolicy::recordEnter(JSContext *cx, JSObject *proxy, jsid id)
{
    if (allowed()) {
        context = cx;
        enteredProxy.construct(cx, proxy);
        enteredId.construct(cx, id);
        prev = cx->runtime->enteredPolicy;
        cx->runtime->enteredPolicy = this;
    }
}

void
js::AutoEnterPolicy::recordLeave()
{
    if (!enteredProxy.empty()) {
        JS_ASSERT(context->runtime->enteredPolicy == this);
        context->runtime->enteredPolicy = prev;
    }
}

JS_FRIEND_API(void)
js::assertEnteredPolicy(JSContext *cx, JSObject *proxy, jsid id)
{
    MOZ_ASSERT(proxy->isProxy());
    MOZ_ASSERT(cx->runtime->enteredPolicy);
    MOZ_ASSERT(cx->runtime->enteredPolicy->enteredProxy.ref().get() == proxy);
    MOZ_ASSERT(cx->runtime->enteredPolicy->enteredId.ref().get() == id);
}
#endif

BaseProxyHandler::BaseProxyHandler(void *family)
  : mFamily(family),
    mHasPrototype(false),
    mHasPolicy(false)
{
}

BaseProxyHandler::~BaseProxyHandler()
{
}

bool
BaseProxyHandler::enter(JSContext *cx, JSObject *wrapper, jsid id, Action act,
                        bool *bp)
{
    *bp = true;
    return true;
}

bool
BaseProxyHandler::has(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    assertEnteredPolicy(cx, proxy_, id_);
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);
    AutoPropertyDescriptorRooter desc(cx);
    if (!getPropertyDescriptor(cx, proxy, id, &desc, 0))
        return false;
    *bp = !!desc.obj;
    return true;
}

bool
BaseProxyHandler::hasOwn(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    assertEnteredPolicy(cx, proxy_, id_);
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);
    AutoPropertyDescriptorRooter desc(cx);
    if (!getOwnPropertyDescriptor(cx, proxy, id, &desc, 0))
        return false;
    *bp = !!desc.obj;
    return true;
}

bool
BaseProxyHandler::get(JSContext *cx, JSObject *proxy, JSObject *receiver_, jsid id_, Value *vp)
{
    assertEnteredPolicy(cx, proxy, id_);
    RootedObject receiver(cx, receiver_);
    RootedId id(cx, id_);

    AutoPropertyDescriptorRooter desc(cx);
    if (!getPropertyDescriptor(cx, proxy, id, &desc, 0))
        return false;
    if (!desc.obj) {
        vp->setUndefined();
        return true;
    }
    if (!desc.getter ||
        (!(desc.attrs & JSPROP_GETTER) && desc.getter == JS_PropertyStub)) {
        *vp = desc.value;
        return true;
    }
    if (desc.attrs & JSPROP_GETTER)
        return InvokeGetterOrSetter(cx, receiver, CastAsObjectJsval(desc.getter), 0, NULL, vp);
    if (!(desc.attrs & JSPROP_SHARED))
        *vp = desc.value;
    else
        vp->setUndefined();
    if (desc.attrs & JSPROP_SHORTID)
        id = INT_TO_JSID(desc.shortid);

    RootedValue value(cx, *vp);
    if (!CallJSPropertyOp(cx, desc.getter, receiver, id, &value))
        return false;

    *vp = value;
    return true;
}

bool
BaseProxyHandler::getElementIfPresent(JSContext *cx, JSObject *proxy_, JSObject *receiver_, uint32_t index, Value *vp, bool *present)
{
    RootedObject proxy(cx, proxy_);
    RootedObject receiver(cx, receiver_);

    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    assertEnteredPolicy(cx, proxy, id);

    if (!has(cx, proxy, id, present))
        return false;

    if (!*present) {
        Debug_SetValueRangeToCrashOnTouch(vp, 1);
        return true;
    }

    return get(cx, proxy, receiver, id, vp);
}

bool
BaseProxyHandler::set(JSContext *cx, JSObject *proxy_, JSObject *receiver_, jsid id_, bool strict,
                      Value *vp)
{
    assertEnteredPolicy(cx, proxy_, id_);
    RootedObject proxy(cx, proxy_), receiver(cx, receiver_);
    RootedId id(cx, id_);

    AutoPropertyDescriptorRooter desc(cx);
    if (!getOwnPropertyDescriptor(cx, proxy, id, &desc, JSRESOLVE_ASSIGNING))
        return false;
    /* The control-flow here differs from ::get() because of the fall-through case below. */
    if (desc.obj) {
        // Check for read-only properties.
        if (desc.attrs & JSPROP_READONLY)
            return strict ? Throw(cx, id, JSMSG_CANT_REDEFINE_PROP) : true;
        if (!desc.setter) {
            // Be wary of the odd explicit undefined setter case possible through
            // Object.defineProperty.
            if (!(desc.attrs & JSPROP_SETTER))
                desc.setter = JS_StrictPropertyStub;
        } else if ((desc.attrs & JSPROP_SETTER) || desc.setter != JS_StrictPropertyStub) {
            RootedValue value(cx, *vp);
            if (!CallSetter(cx, receiver, id, desc.setter, desc.attrs, desc.shortid, strict, &value))
                return false;
            *vp = value;
            if (!proxy->isProxy() || GetProxyHandler(proxy) != this)
                return true;
            if (desc.attrs & JSPROP_SHARED)
                return true;
        }
        if (!desc.getter) {
            // Same as above for the null setter case.
            if (!(desc.attrs & JSPROP_GETTER))
                desc.getter = JS_PropertyStub;
        }
        desc.value = *vp;
        return defineProperty(cx, receiver, id, &desc);
    }
    if (!getPropertyDescriptor(cx, proxy, id, &desc, JSRESOLVE_ASSIGNING))
        return false;
    if (desc.obj) {
        // Check for read-only properties.
        if (desc.attrs & JSPROP_READONLY)
            return strict ? Throw(cx, id, JSMSG_CANT_REDEFINE_PROP) : true;
        if (!desc.setter) {
            // Be wary of the odd explicit undefined setter case possible through
            // Object.defineProperty.
            if (!(desc.attrs & JSPROP_SETTER))
                desc.setter = JS_StrictPropertyStub;
        } else if ((desc.attrs & JSPROP_SETTER) || desc.setter != JS_StrictPropertyStub) {
            RootedValue value(cx, *vp);
            if (!CallSetter(cx, receiver, id, desc.setter, desc.attrs, desc.shortid, strict, &value))
                return false;
            *vp = value;
            if (!proxy->isProxy() || GetProxyHandler(proxy) != this)
                return true;
            if (desc.attrs & JSPROP_SHARED)
                return true;
        }
        if (!desc.getter) {
            // Same as above for the null setter case.
            if (!(desc.attrs & JSPROP_GETTER))
                desc.getter = JS_PropertyStub;
        }
        desc.value = *vp;
        return defineProperty(cx, receiver, id, &desc);
    }

    desc.obj = receiver;
    desc.value = *vp;
    desc.attrs = JSPROP_ENUMERATE;
    desc.shortid = 0;
    desc.getter = NULL;
    desc.setter = NULL; // Pick up the class getter/setter.
    return defineProperty(cx, receiver, id, &desc);
}

bool
BaseProxyHandler::keys(JSContext *cx, JSObject *proxyArg, AutoIdVector &props)
{
    assertEnteredPolicy(cx, proxyArg, JSID_VOID);
    JS_ASSERT(props.length() == 0);

    RootedObject proxy(cx, proxyArg);

    if (!getOwnPropertyNames(cx, proxy, props))
        return false;

    /* Select only the enumerable properties through in-place iteration. */
    AutoPropertyDescriptorRooter desc(cx);
    size_t i = 0;
    for (size_t j = 0, len = props.length(); j < len; j++) {
        JS_ASSERT(i <= j);
        jsid id = props[j];
        AutoWaivePolicy policy(cx, proxy, id);
        if (!getOwnPropertyDescriptor(cx, proxy, id, &desc, 0))
            return false;
        if (desc.obj && (desc.attrs & JSPROP_ENUMERATE))
            props[i++] = id;
    }

    JS_ASSERT(i <= props.length());
    props.resize(i);

    return true;
}

bool
BaseProxyHandler::iterate(JSContext *cx, JSObject *proxy_, unsigned flags, Value *vp)
{
    assertEnteredPolicy(cx, proxy_, JSID_VOID);
    RootedObject proxy(cx, proxy_);

    AutoIdVector props(cx);
    if ((flags & JSITER_OWNONLY)
        ? !keys(cx, proxy, props)
        : !enumerate(cx, proxy, props)) {
        return false;
    }

    RootedValue value(cx);
    if (!EnumeratedIdVectorToIterator(cx, proxy, flags, props, &value))
        return false;

    *vp = value;
    return true;
}

bool
BaseProxyHandler::call(JSContext *cx, JSObject *proxy, unsigned argc,
                       Value *vp)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    AutoValueRooter rval(cx);
    RootedValue call(cx, GetCall(proxy));
    JSBool ok = Invoke(cx, vp[1], call, argc, JS_ARGV(cx, vp), rval.addr());
    if (ok)
        JS_SET_RVAL(cx, vp, rval.value());
    return ok;
}

bool
BaseProxyHandler::construct(JSContext *cx, JSObject *proxy_, unsigned argc,
                            Value *argv, Value *rval)
{
    assertEnteredPolicy(cx, proxy_, JSID_VOID);
    RootedObject proxy(cx, proxy_);
    RootedValue fval(cx, GetConstruct(proxy_));
    if (fval.isUndefined())
        fval = GetCall(proxy);
    return InvokeConstructor(cx, fval, argc, argv, rval);
}

JSString *
BaseProxyHandler::obj_toString(JSContext *cx, JSObject *proxy)
{
    return JS_NewStringCopyZ(cx, IsFunctionProxy(proxy)
                                 ? "[object Function]"
                                 : "[object Object]");
}

JSString *
BaseProxyHandler::fun_toString(JSContext *cx, JSObject *proxy, unsigned indent)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    Value fval = GetCall(proxy);
    if (IsFunctionProxy(proxy) &&
        (fval.isPrimitive() || !fval.toObject().isFunction())) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             JSMSG_INCOMPATIBLE_PROTO,
                             js_Function_str, js_toString_str,
                             "object");
        return NULL;
    }
    RootedObject obj(cx, &fval.toObject());
    return fun_toStringHelper(cx, obj, indent);

}

bool
BaseProxyHandler::regexp_toShared(JSContext *cx, JSObject *proxy,
                                  RegExpGuard *g)
{
    JS_NOT_REACHED("This should have been a wrapped regexp");
    return false;
}

bool
BaseProxyHandler::defaultValue(JSContext *cx, JSObject *proxy, JSType hint,
                               Value *vp)
{
    Rooted<JSObject*> obj(cx, proxy);
    RootedValue value(cx);
    if (!DefaultValue(cx, obj, hint, &value))
        return false;

    *vp = value;
    return true;
}

bool
BaseProxyHandler::nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl, CallArgs args)
{
    ReportIncompatible(cx, args);
    return false;
}

bool
BaseProxyHandler::hasInstance(JSContext *cx, HandleObject proxy, MutableHandleValue v, bool *bp)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    RootedValue val(cx, ObjectValue(*proxy.get()));
    js_ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS,
                        JSDVG_SEARCH_STACK, val, NullPtr());
    return false;
}

bool
BaseProxyHandler::objectClassIs(JSObject *proxy, ESClassValue classValue, JSContext *cx)
{
    return false;
}

void
BaseProxyHandler::finalize(JSFreeOp *fop, JSObject *proxy)
{
}

JSObject *
BaseProxyHandler::weakmapKeyDelegate(JSObject *proxy)
{
    return NULL;
}

bool
BaseProxyHandler::getPrototypeOf(JSContext *cx, JSObject *proxy, JSObject **proto)
{
    // The default implementation here just uses proto of the proxy object.
    *proto = proxy->getTaggedProto().toObjectOrNull();
    return true;
}


bool
DirectProxyHandler::getPropertyDescriptor(JSContext *cx, JSObject *proxy,
                                          jsid id, PropertyDescriptor *desc, unsigned flags)
{
    assertEnteredPolicy(cx, proxy, id);
    JS_ASSERT(!hasPrototype()); // Should never be called if there's a prototype.
    RootedObject target(cx, GetProxyTargetObject(proxy));
    return JS_GetPropertyDescriptorById(cx, target, id, 0, desc);
}

static bool
GetOwnPropertyDescriptor(JSContext *cx, HandleObject obj, jsid id, unsigned flags,
                         JSPropertyDescriptor *desc)
{
    // If obj is a proxy, we can do better than just guessing. This is
    // important for certain types of wrappers that wrap other wrappers.
    if (obj->isProxy())
        return Proxy::getOwnPropertyDescriptor(cx, obj, id, desc, flags);

    if (!JS_GetPropertyDescriptorById(cx, obj, id, flags, desc))
        return false;
    if (desc->obj != obj)
        desc->obj = NULL;
    return true;
}

bool
DirectProxyHandler::getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy,
                                             jsid id, PropertyDescriptor *desc, unsigned flags)
{
    assertEnteredPolicy(cx, proxy, id);
    RootedObject target(cx, GetProxyTargetObject(proxy));
    return GetOwnPropertyDescriptor(cx, target, id, 0, desc);
}

bool
DirectProxyHandler::defineProperty(JSContext *cx, JSObject *proxy, jsid id_,
                                   PropertyDescriptor *desc)
{
    assertEnteredPolicy(cx, proxy, id_);
    RootedObject obj(cx, GetProxyTargetObject(proxy));
    Rooted<jsid> id(cx, id_);
    Rooted<Value> v(cx, desc->value);
    return CheckDefineProperty(cx, obj, id, v, desc->getter, desc->setter, desc->attrs) &&
           JS_DefinePropertyById(cx, obj, id, v, desc->getter, desc->setter, desc->attrs);
}

bool
DirectProxyHandler::getOwnPropertyNames(JSContext *cx, JSObject *proxy,
                                        AutoIdVector &props)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    RootedObject target(cx, GetProxyTargetObject(proxy));
    return GetPropertyNames(cx, target, JSITER_OWNONLY | JSITER_HIDDEN, &props);
}

bool
DirectProxyHandler::delete_(JSContext *cx, JSObject *proxy, jsid id, bool *bp)
{
    RootedValue v(cx);
    assertEnteredPolicy(cx, proxy, id);
    RootedObject target(cx, GetProxyTargetObject(proxy));
    if (!JS_DeletePropertyById2(cx, target, id, v.address()))
        return false;
    JSBool b;
    if (!JS_ValueToBoolean(cx, v, &b))
        return false;
    *bp = !!b;
    return true;
}

bool
DirectProxyHandler::enumerate(JSContext *cx, JSObject *proxy,
                              AutoIdVector &props)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    JS_ASSERT(!hasPrototype()); // Should never be called if there's a prototype.
    RootedObject target(cx, GetProxyTargetObject(proxy));
    return GetPropertyNames(cx, target, 0, &props);
}

bool
DirectProxyHandler::nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                               CallArgs args)
{
    args.setThis(ObjectValue(*GetProxyTargetObject(&args.thisv().toObject())));
    if (!test(args.thisv())) {
        ReportIncompatible(cx, args);
        return false;
    }

    return CallNativeImpl(cx, impl, args);
}

bool
DirectProxyHandler::hasInstance(JSContext *cx, HandleObject proxy, MutableHandleValue v,
                                bool *bp)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    JSBool b;
    RootedObject target(cx, GetProxyTargetObject(proxy));
    if (!JS_HasInstance(cx, target, v, &b))
        return false;
    *bp = !!b;
    return true;
}


bool
DirectProxyHandler::objectClassIs(JSObject *proxy, ESClassValue classValue,
                                  JSContext *cx)
{
    RootedObject obj(cx, GetProxyTargetObject(proxy));
    return ObjectClassIs(obj, classValue, cx);
}

JSString *
DirectProxyHandler::obj_toString(JSContext *cx, JSObject *proxy)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    return obj_toStringHelper(cx, GetProxyTargetObject(proxy));
}

JSString *
DirectProxyHandler::fun_toString(JSContext *cx, JSObject *proxy,
                                 unsigned indent)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    RootedObject target(cx, GetProxyTargetObject(proxy));
    return fun_toStringHelper(cx, target, indent);
}

bool
DirectProxyHandler::regexp_toShared(JSContext *cx, JSObject *proxy,
                                    RegExpGuard *g)
{
    return RegExpToShared(cx, *GetProxyTargetObject(proxy), g);
}

bool
DirectProxyHandler::defaultValue(JSContext *cx, JSObject *proxy, JSType hint, Value *vp)
{
    *vp = ObjectValue(*GetProxyTargetObject(proxy));
    RootedValue tmp(cx, *vp);
    bool ok = hint == JSTYPE_VOID ? ToPrimitive(cx, &tmp) : ToPrimitive(cx, hint, &tmp);
    *vp = tmp;
    return ok;
}

JSObject *
DirectProxyHandler::weakmapKeyDelegate(JSObject *proxy)
{
    return UnwrapObject(proxy);
}

DirectProxyHandler::DirectProxyHandler(void *family)
  : BaseProxyHandler(family)
{
}

bool
DirectProxyHandler::has(JSContext *cx, JSObject *proxy, jsid id, bool *bp)
{
    assertEnteredPolicy(cx, proxy, id);
    JS_ASSERT(!hasPrototype()); // Should never be called if there's a prototype.
    JSBool found;
    RootedObject target(cx, GetProxyTargetObject(proxy));
    if (!JS_HasPropertyById(cx, target, id, &found))
        return false;
    *bp = !!found;
    return true;
}

bool
DirectProxyHandler::hasOwn(JSContext *cx, JSObject *proxy, jsid id, bool *bp)
{
    assertEnteredPolicy(cx, proxy, id);
    RootedObject target(cx, GetProxyTargetObject(proxy));
    AutoPropertyDescriptorRooter desc(cx);
    if (!JS_GetPropertyDescriptorById(cx, target, id, 0, &desc))
        return false;
    *bp = (desc.obj == target);
    return true;
}

bool
DirectProxyHandler::get(JSContext *cx, JSObject *proxy, JSObject *receiver_,
                        jsid id_, Value *vp)
{
    assertEnteredPolicy(cx, proxy, id_);
    RootedObject receiver(cx, receiver_);
    RootedId id(cx, id_);
    RootedValue value(cx);
    RootedObject target(cx, GetProxyTargetObject(proxy));
    if (!JSObject::getGeneric(cx, target, receiver, id, &value))
        return false;

    *vp = value;
    return true;
}

bool
DirectProxyHandler::set(JSContext *cx, JSObject *proxy, JSObject *receiverArg,
                        jsid id_, bool strict, Value *vp)
{
    assertEnteredPolicy(cx, proxy, id_);
    RootedId id(cx, id_);
    Rooted<JSObject*> receiver(cx, receiverArg);
    RootedValue value(cx, *vp);
    RootedObject target(cx, GetProxyTargetObject(proxy));
    if (!JSObject::setGeneric(cx, target, receiver, id, &value, strict))
        return false;

    *vp = value;
    return true;
}

bool
DirectProxyHandler::keys(JSContext *cx, JSObject *proxy, AutoIdVector &props)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    return GetPropertyNames(cx, GetProxyTargetObject(proxy), JSITER_OWNONLY, &props);
}

bool
DirectProxyHandler::iterate(JSContext *cx, JSObject *proxy, unsigned flags,
                            Value *vp)
{
    assertEnteredPolicy(cx, proxy, JSID_VOID);
    JS_ASSERT(!hasPrototype()); // Should never be called if there's a prototype.
    Rooted<JSObject*> target(cx, GetProxyTargetObject(proxy));
    RootedValue value(cx);
    if (!GetIterator(cx, target, flags, &value))
        return false;

    *vp = value;
    return true;
}

static bool
GetFundamentalTrap(JSContext *cx, HandleObject handler, HandlePropertyName name,
                   MutableHandleValue fvalp)
{
    JS_CHECK_RECURSION(cx, return false);

    return JSObject::getProperty(cx, handler, handler, name, fvalp);
}

static bool
GetDerivedTrap(JSContext *cx, HandleObject handler, HandlePropertyName name,
               MutableHandleValue fvalp)
{
    JS_ASSERT(name == cx->names().has ||
              name == cx->names().hasOwn ||
              name == cx->names().get ||
              name == cx->names().set ||
              name == cx->names().keys ||
              name == cx->names().iterate);

    return JSObject::getProperty(cx, handler, handler, name, fvalp);
}

static bool
Trap(JSContext *cx, HandleObject handler, HandleValue fval, unsigned argc, Value* argv, Value *rval)
{
    return Invoke(cx, ObjectValue(*handler), fval, argc, argv, rval);
}

static bool
Trap1(JSContext *cx, HandleObject handler, HandleValue fval, HandleId id, Value *rval)
{
    JSString *str = ToString<CanGC>(cx, IdToValue(id));
    if (!str)
        return false;
    rval->setString(str);
    return Trap(cx, handler, fval, 1, rval, rval);
}

static bool
Trap2(JSContext *cx, HandleObject handler, HandleValue fval, HandleId id, Value v_, Value *rval)
{
    RootedValue v(cx, v_);
    JSString *str = ToString<CanGC>(cx, IdToValue(id));
    if (!str)
        return false;
    rval->setString(str);
    Value argv[2] = { *rval, v };
    AutoValueArray ava(cx, argv, 2);
    return Trap(cx, handler, fval, 2, argv, rval);
}

static bool
ParsePropertyDescriptorObject(JSContext *cx, HandleObject obj, const Value &v,
                              PropertyDescriptor *desc, bool complete = false)
{
    AutoPropDescArrayRooter descs(cx);
    PropDesc *d = descs.append();
    if (!d || !d->initialize(cx, v))
        return false;
    if (complete)
        d->complete();
    desc->obj = obj;
    desc->value = d->hasValue() ? d->value() : UndefinedValue();
    JS_ASSERT(!(d->attributes() & JSPROP_SHORTID));
    desc->attrs = d->attributes();
    desc->getter = d->getter();
    desc->setter = d->setter();
    desc->shortid = 0;
    return true;
}

static bool
IndicatePropertyNotFound(PropertyDescriptor *desc)
{
    desc->obj = NULL;
    return true;
}

static bool
ValueToBool(const Value &v, bool *bp)
{
    *bp = ToBoolean(v);
    return true;
}

static bool
ArrayToIdVector(JSContext *cx, const Value &array, AutoIdVector &props)
{
    JS_ASSERT(props.length() == 0);

    if (array.isPrimitive())
        return true;

    RootedObject obj(cx, &array.toObject());
    uint32_t length;
    if (!GetLengthProperty(cx, obj, &length))
        return false;

    RootedValue v(cx);
    for (uint32_t n = 0; n < length; ++n) {
        if (!JS_CHECK_OPERATION_LIMIT(cx))
            return false;
        if (!JSObject::getElement(cx, obj, obj, n, &v))
            return false;
        RootedId id(cx);
        if (!ValueToId<CanGC>(cx, v, &id))
            return false;
        if (!props.append(id))
            return false;
    }

    return true;
}

/* Derived class for all scripted indirect proxy handlers. */
class ScriptedIndirectProxyHandler : public BaseProxyHandler {
  public:
    ScriptedIndirectProxyHandler();
    virtual ~ScriptedIndirectProxyHandler();

    /* ES5 Harmony fundamental proxy traps. */
    virtual bool getPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                       PropertyDescriptor *desc, unsigned flags) MOZ_OVERRIDE;
    virtual bool getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                          PropertyDescriptor *desc, unsigned flags) MOZ_OVERRIDE;
    virtual bool defineProperty(JSContext *cx, JSObject *proxy, jsid id,
                                PropertyDescriptor *desc) MOZ_OVERRIDE;
    virtual bool getOwnPropertyNames(JSContext *cx, JSObject *proxy, AutoIdVector &props);
    virtual bool delete_(JSContext *cx, JSObject *proxy, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool enumerate(JSContext *cx, JSObject *proxy, AutoIdVector &props) MOZ_OVERRIDE;

    /* ES5 Harmony derived proxy traps. */
    virtual bool has(JSContext *cx, JSObject *proxy, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool hasOwn(JSContext *cx, JSObject *proxy, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool get(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id,
                     Value *vp) MOZ_OVERRIDE;
    virtual bool set(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id, bool strict,
                     Value *vp) MOZ_OVERRIDE;
    virtual bool keys(JSContext *cx, JSObject *proxy, AutoIdVector &props) MOZ_OVERRIDE;
    virtual bool iterate(JSContext *cx, JSObject *proxy, unsigned flags, Value *vp) MOZ_OVERRIDE;

    /* Spidermonkey extensions. */
    virtual bool nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                            CallArgs args) MOZ_OVERRIDE;
    virtual bool defaultValue(JSContext *cx, JSObject *obj, JSType hint, Value *vp) MOZ_OVERRIDE;

    static ScriptedIndirectProxyHandler singleton;
};

static int sScriptedIndirectProxyHandlerFamily = 0;

ScriptedIndirectProxyHandler::ScriptedIndirectProxyHandler()
        : BaseProxyHandler(&sScriptedIndirectProxyHandlerFamily)
{
}

ScriptedIndirectProxyHandler::~ScriptedIndirectProxyHandler()
{
}

static bool
ReturnedValueMustNotBePrimitive(JSContext *cx, HandleObject proxy, JSAtom *atom, const Value &v)
{
    if (v.isPrimitive()) {
        JSAutoByteString bytes;
        if (js_AtomToPrintableString(cx, atom, &bytes)) {
            RootedValue val(cx, ObjectOrNullValue(proxy));
            js_ReportValueError2(cx, JSMSG_BAD_TRAP_RETURN_VALUE,
                                 JSDVG_SEARCH_STACK, val, NullPtr(), bytes.ptr());
        }
        return false;
    }
    return true;
}

static JSObject *
GetIndirectProxyHandlerObject(JSObject *proxy)
{
    return GetProxyPrivate(proxy).toObjectOrNull();
}

bool
ScriptedIndirectProxyHandler::getPropertyDescriptor(JSContext *cx, JSObject *proxy_, jsid id_,
                                                    PropertyDescriptor *desc, unsigned flags)
{
    RootedId id(cx, id_);
    RootedObject proxy(cx, proxy_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().getPropertyDescriptor, &fval) &&
           Trap1(cx, handler, fval, id, value.address()) &&
           ((value.get().isUndefined() && IndicatePropertyNotFound(desc)) ||
            (ReturnedValueMustNotBePrimitive(cx, proxy, cx->names().getPropertyDescriptor, value) &&
             ParsePropertyDescriptorObject(cx, proxy, value, desc)));
}

bool
ScriptedIndirectProxyHandler::getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy_, jsid id_,
                                                       PropertyDescriptor *desc, unsigned flags)
{
    RootedId id(cx, id_);
    RootedObject proxy(cx, proxy_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().getOwnPropertyDescriptor, &fval) &&
           Trap1(cx, handler, fval, id, value.address()) &&
           ((value.get().isUndefined() && IndicatePropertyNotFound(desc)) ||
            (ReturnedValueMustNotBePrimitive(cx, proxy, cx->names().getPropertyDescriptor, value) &&
             ParsePropertyDescriptorObject(cx, proxy, value, desc)));
}

bool
ScriptedIndirectProxyHandler::defineProperty(JSContext *cx, JSObject *proxy, jsid id_,
                                             PropertyDescriptor *desc)
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    RootedId id(cx, id_);
    return GetFundamentalTrap(cx, handler, cx->names().defineProperty, &fval) &&
           NewPropertyDescriptorObject(cx, desc, &value) &&
           Trap2(cx, handler, fval, id, value, value.address());
}

bool
ScriptedIndirectProxyHandler::getOwnPropertyNames(JSContext *cx, JSObject *proxy,
                                                  AutoIdVector &props)
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().getOwnPropertyNames, &fval) &&
           Trap(cx, handler, fval, 0, NULL, value.address()) &&
           ArrayToIdVector(cx, value, props);
}

bool
ScriptedIndirectProxyHandler::delete_(JSContext *cx, JSObject *proxy, jsid id_, bool *bp)
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedId id(cx, id_);
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().delete_, &fval) &&
           Trap1(cx, handler, fval, id, value.address()) &&
           ValueToBool(value, bp);
}

bool
ScriptedIndirectProxyHandler::enumerate(JSContext *cx, JSObject *proxy, AutoIdVector &props)
{
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    return GetFundamentalTrap(cx, handler, cx->names().enumerate, &fval) &&
           Trap(cx, handler, fval, 0, NULL, value.address()) &&
           ArrayToIdVector(cx, value, props);
}

bool
ScriptedIndirectProxyHandler::has(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().has, &fval))
        return false;
    if (!js_IsCallable(fval))
        return BaseProxyHandler::has(cx, proxy, id, bp);
    return Trap1(cx, handler, fval, id, value.address()) &&
           ValueToBool(value, bp);
}

bool
ScriptedIndirectProxyHandler::hasOwn(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue fval(cx), value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().hasOwn, &fval))
        return false;
    if (!js_IsCallable(fval))
        return BaseProxyHandler::hasOwn(cx, proxy, id, bp);
    return Trap1(cx, handler, fval, id, value.address()) &&
           ValueToBool(value, bp);
}

bool
ScriptedIndirectProxyHandler::get(JSContext *cx, JSObject *proxy_, JSObject *receiver_, jsid id_,
                                  Value *vp)
{
    RootedId id(cx, id_);
    RootedObject proxy(cx, proxy_), receiver(cx, receiver_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    JSString *str = ToString<CanGC>(cx, IdToValue(id));
    if (!str)
        return false;
    RootedValue value(cx, StringValue(str));
    Value argv[] = { ObjectOrNullValue(receiver), value };
    AutoValueArray ava(cx, argv, 2);
    RootedValue fval(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().get, &fval))
        return false;
    if (!js_IsCallable(fval))
        return BaseProxyHandler::get(cx, proxy, receiver, id, vp);
    return Trap(cx, handler, fval, 2, argv, vp);
}

bool
ScriptedIndirectProxyHandler::set(JSContext *cx, JSObject *proxy_, JSObject *receiver_, jsid id_, bool strict,
                                  Value *vp)
{
    RootedId id(cx, id_);
    RootedObject proxy(cx, proxy_), receiver(cx, receiver_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    JSString *str = ToString<CanGC>(cx, IdToValue(id));
    if (!str)
        return false;
    RootedValue value(cx, StringValue(str));
    Value argv[] = { ObjectOrNullValue(receiver), value, *vp };
    AutoValueArray ava(cx, argv, 3);
    RootedValue fval(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().set, &fval))
        return false;
    if (!js_IsCallable(fval))
        return BaseProxyHandler::set(cx, proxy, receiver, id, strict, vp);
    return Trap(cx, handler, fval, 3, argv, value.address());
}

bool
ScriptedIndirectProxyHandler::keys(JSContext *cx, JSObject *proxy_, AutoIdVector &props)
{
    RootedObject proxy(cx, proxy_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().keys, &value))
        return false;
    if (!js_IsCallable(value))
        return BaseProxyHandler::keys(cx, proxy, props);
    return Trap(cx, handler, value, 0, NULL, value.address()) &&
           ArrayToIdVector(cx, value, props);
}

bool
ScriptedIndirectProxyHandler::iterate(JSContext *cx, JSObject *proxy_, unsigned flags, Value *vp)
{
    RootedObject proxy(cx, proxy_);
    RootedObject handler(cx, GetIndirectProxyHandlerObject(proxy));
    RootedValue value(cx);
    if (!GetDerivedTrap(cx, handler, cx->names().iterate, &value))
        return false;
    if (!js_IsCallable(value))
        return BaseProxyHandler::iterate(cx, proxy, flags, vp);
    return Trap(cx, handler, value, 0, NULL, vp) &&
           ReturnedValueMustNotBePrimitive(cx, proxy, cx->names().iterate, *vp);
}

bool
ScriptedIndirectProxyHandler::nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                                         CallArgs args)
{
    return BaseProxyHandler::nativeCall(cx, test, impl, args);
}

bool
ScriptedIndirectProxyHandler::defaultValue(JSContext *cx, JSObject *proxy, JSType hint, Value *vp)
{
    /*
     * This function is only here to prevent bug 757063. It will be removed when
     * the direct proxy refactor is complete.
     */
    return BaseProxyHandler::defaultValue(cx, proxy, hint, vp);
}

ScriptedIndirectProxyHandler ScriptedIndirectProxyHandler::singleton;

static JSObject *
GetDirectProxyHandlerObject(JSObject *proxy)
{
    return GetProxyExtra(proxy, 0).toObjectOrNull();
}

/* Derived class for all scripted direct proxy handlers. */
class ScriptedDirectProxyHandler : public DirectProxyHandler {
  public:
    ScriptedDirectProxyHandler();
    virtual ~ScriptedDirectProxyHandler();

    /* ES5 Harmony fundamental proxy traps. */
    virtual bool getPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                       PropertyDescriptor *desc, unsigned flags) MOZ_OVERRIDE;
    virtual bool getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy, jsid id,
                                          PropertyDescriptor *desc, unsigned flags) MOZ_OVERRIDE;
    virtual bool defineProperty(JSContext *cx, JSObject *proxy, jsid id,
                                PropertyDescriptor *desc) MOZ_OVERRIDE;
    virtual bool getOwnPropertyNames(JSContext *cx, JSObject *proxy, AutoIdVector &props);
    virtual bool delete_(JSContext *cx, JSObject *proxy, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool enumerate(JSContext *cx, JSObject *proxy, AutoIdVector &props) MOZ_OVERRIDE;

    /* ES5 Harmony derived proxy traps. */
    virtual bool has(JSContext *cx, JSObject *proxy, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool hasOwn(JSContext *cx, JSObject *proxy, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool get(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id,
                     Value *vp) MOZ_OVERRIDE;
    virtual bool set(JSContext *cx, JSObject *proxy, JSObject *receiver, jsid id, bool strict,
                     Value *vp) MOZ_OVERRIDE;
    virtual bool keys(JSContext *cx, JSObject *proxy, AutoIdVector &props) MOZ_OVERRIDE;
    virtual bool iterate(JSContext *cx, JSObject *proxy, unsigned flags, Value *vp) MOZ_OVERRIDE;

    virtual bool call(JSContext *cx, JSObject *proxy, unsigned argc, Value *vp) MOZ_OVERRIDE;
    virtual bool construct(JSContext *cx, JSObject *proxy, unsigned argc, Value *argv,
                           Value *rval) MOZ_OVERRIDE;

    static ScriptedDirectProxyHandler singleton;
};

static int sScriptedDirectProxyHandlerFamily = 0;

// Aux.2 FromGenericPropertyDescriptor(Desc)
static bool
FromGenericPropertyDescriptor(JSContext *cx, PropDesc *desc, MutableHandleValue rval)
{
    // Aux.2 step 1
    if (desc->isUndefined()) {
        rval.setUndefined();
        return true;
    }

    // steps 3-9
    if (!desc->makeObject(cx))
        return false;
    rval.set(desc->pd());
    return true;
}

/*
 * Aux.3 NormalizePropertyDescriptor(Attributes)
 *
 * NOTE: to minimize code duplication, the code for this function is shared with
 * that for Aux.4 NormalizeAndCompletePropertyDescriptor (see below). The
 * argument complete is used to distinguish between the two.
 */
static bool
NormalizePropertyDescriptor(JSContext *cx, MutableHandleValue vp, bool complete = false)
{
    // Aux.4 step 1
    if (complete && vp.isUndefined())
        return true;

    // Aux.3 steps 1-2 / Aux.4 steps 2-3
    AutoPropDescArrayRooter descs(cx);
    PropDesc *desc = descs.append();
    if (!desc || !desc->initialize(cx, vp.get()))
        return false;
    if (complete)
        desc->complete();
    JS_ASSERT(!vp.isPrimitive()); // due to desc->initialize
    RootedObject attributes(cx, &vp.toObject());

    /*
     * Aux.3 step 3 / Aux.4 step 4
     *
     * NOTE: Aux.4 step 4 actually specifies FromPropertyDescriptor here.
     * However, the way FromPropertyDescriptor is implemented (PropDesc::
     * makeObject) is actually closer to FromGenericPropertyDescriptor,
     * and is in fact used to implement the latter, so we might as well call it
     * directly.
     */
    if (!FromGenericPropertyDescriptor(cx, desc, vp))
        return false;
    if (vp.isUndefined())
        return true;
    RootedObject descObj(cx, &vp.toObject());

    // Aux.3 steps 4-5 / Aux.4 steps 5-6
    AutoIdVector props(cx);
    if (!GetPropertyNames(cx, attributes, 0, &props))
        return false;
    size_t n = props.length();
    for (size_t i = 0; i < n; ++i) {
        RootedId id(cx, props[i]);
        if (JSID_IS_ATOM(id)) {
            JSAtom *atom = JSID_TO_ATOM(id);
            const JSAtomState &atomState = cx->runtime->atomState;
            if (atom == atomState.value || atom == atomState.writable ||
                atom == atomState.get || atom == atomState.set ||
                atom == atomState.enumerable || atom == atomState.configurable)
            {
                continue;
            }
        }

        RootedValue v(cx);
        if (!JSObject::getGeneric(cx, descObj, attributes, id, &v))
            return false;
        if (!JSObject::defineGeneric(cx, descObj, id, v, NULL, NULL, JSPROP_ENUMERATE))
            return false;
    }
    return true;
}

// Aux.4 NormalizeAndCompletePropertyDescriptor(Attributes)
static inline bool
NormalizeAndCompletePropertyDescriptor(JSContext *cx, MutableHandleValue vp)
{
    return NormalizePropertyDescriptor(cx, vp, true);
}

static inline bool
IsDataDescriptor(const PropertyDescriptor &desc)
{
    return desc.obj && !(desc.attrs & (JSPROP_GETTER | JSPROP_SETTER));
}

static inline bool
IsAccessorDescriptor(const PropertyDescriptor &desc)
{
    return desc.obj && desc.attrs & (JSPROP_GETTER | JSPROP_SETTER);
}

// Aux.5 ValidateProperty(O, P, Desc)
static bool
ValidateProperty(JSContext *cx, HandleObject obj, HandleId id, PropDesc *desc, bool *bp)
{
    // step 1
    AutoPropertyDescriptorRooter current(cx);
    if (!GetOwnPropertyDescriptor(cx, obj, id, 0, &current))
        return false;

    /*
     * steps 2-4 are redundant since ValidateProperty is never called unless
     * target.[[HasOwn]](P) is true
     */
    JS_ASSERT(current.obj);

    // step 5
    if (!desc->hasValue() && !desc->hasWritable() && !desc->hasGet() && !desc->hasSet() &&
        !desc->hasEnumerable() && !desc->hasConfigurable())
    {
        *bp = true;
        return true;
    }

    // step 6
    if ((!desc->hasWritable() || desc->writable() == !(current.attrs & JSPROP_READONLY)) &&
        (!desc->hasGet() || desc->getter() == current.getter) &&
        (!desc->hasSet() || desc->setter() == current.setter) &&
        (!desc->hasEnumerable() || desc->enumerable() == bool(current.attrs & JSPROP_ENUMERATE)) &&
        (!desc->hasConfigurable() || desc->configurable() == !(current.attrs & JSPROP_PERMANENT)))
    {
        if (!desc->hasValue()) {
            *bp = true;
            return true;
        }
        bool same = false;
        if (!SameValue(cx, desc->value(), current.value, &same))
            return false;
        if (same) {
            *bp = true;
            return true;
        }
    }

    // step 7
    if (current.attrs & JSPROP_PERMANENT) {
        if (desc->hasConfigurable() && desc->configurable()) {
            *bp = false;
            return true;
        }

        if (desc->hasEnumerable() &&
            desc->enumerable() != bool(current.attrs & JSPROP_ENUMERATE))
        {
            *bp = false;
            return true;
        }
    }

    // step 8
    if (desc->isGenericDescriptor()) {
        *bp = true;
        return true;
    }

    // step 9
    if (IsDataDescriptor(current) != desc->isDataDescriptor()) {
        *bp = !(current.attrs & JSPROP_PERMANENT);
        return true;
    }

    // step 10
    if (IsDataDescriptor(current)) {
        JS_ASSERT(desc->isDataDescriptor()); // by step 9
        if ((current.attrs & JSPROP_PERMANENT) && (current.attrs & JSPROP_READONLY)) {
            if (desc->hasWritable() && desc->writable()) {
                *bp = false;
                return true;
            }

            if (desc->hasValue()) {
                bool same;
                if (!SameValue(cx, desc->value(), current.value, &same))
                    return false;
                if (!same) {
                    *bp = false;
                    return true;
                }
            }
        }

        *bp = true;
        return true;
    }

    // steps 11-12
    JS_ASSERT(IsAccessorDescriptor(current)); // by step 10
    JS_ASSERT(desc->isAccessorDescriptor()); // by step 9
    *bp = (!(current.attrs & JSPROP_PERMANENT) ||
           ((!desc->hasSet() || desc->setter() == current.setter) &&
            (!desc->hasGet() || desc->getter() == current.getter)));
    return true;
}

// Aux.6 IsSealed(O, P)
static bool
IsSealed(JSContext* cx, HandleObject obj, HandleId id, bool *bp)
{
    // step 1
    AutoPropertyDescriptorRooter desc(cx);
    if (!GetOwnPropertyDescriptor(cx, obj, id, 0, &desc))
        return false;

    // steps 2-3
    *bp = desc.obj && (desc.attrs & JSPROP_PERMANENT);
    return true;
}

static bool
HasOwn(JSContext *cx, HandleObject obj, HandleId id, bool *bp)
{
    AutoPropertyDescriptorRooter desc(cx);
    if (!JS_GetPropertyDescriptorById(cx, obj, id, 0, &desc))
        return false;
    *bp = (desc.obj == obj);
    return true;
}

static bool
IdToValue(JSContext *cx, HandleId id, MutableHandleValue value)
{
    JSString *name = ToString<CanGC>(cx, IdToValue(id));
    if (!name)
        return false;
    value.set(StringValue(name));
    return true;
}

// TrapGetOwnProperty(O, P)
static bool
TrapGetOwnProperty(JSContext *cx, HandleObject proxy, HandleId id, MutableHandleValue rval)
{
    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step 3
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().getOwnPropertyDescriptor, &trap))
        return false;

    // step 4
    if (trap.isUndefined()) {
        AutoPropertyDescriptorRooter desc(cx);
        if (!GetOwnPropertyDescriptor(cx, target, id, &desc))
            return false;
        return NewPropertyDescriptorObject(cx, &desc, rval);
    }

    // step 5
    RootedValue value(cx);
    if (!IdToValue(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectValue(*target),
        value
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 2, argv, trapResult.address()))
        return false;

    // step 6
    if (!NormalizeAndCompletePropertyDescriptor(cx, &trapResult))
        return false;

    // step 7
    if (trapResult.isUndefined()) {
        bool sealed;
        if (!IsSealed(cx, target, id, &sealed))
            return false;
        if (sealed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_NC_AS_NE);
            return false;
        }

        if (!target->isExtensible()) {
            bool found;
            if (!HasOwn(cx, target, id, &found))
                return false;
            if (found) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_E_AS_NE);
                return false;
            }
        }

        rval.set(UndefinedValue());
        return true;
    }

    // step 8
    bool isFixed;
    if (!HasOwn(cx, target, id, &isFixed))
        return false;

    // step 9
    if (target->isExtensible() && !isFixed) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_NEW);
        return false;
    }

    AutoPropDescArrayRooter descs(cx);
    PropDesc *desc = descs.append();
    if (!desc || !desc->initialize(cx, trapResult))
        return false;

    /* step 10 */
    if (isFixed) {
        bool valid;
        if (!ValidateProperty(cx, target, id, desc, &valid))
            return false;

        if (!valid) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_INVALID);
            return false;
        }
    }

    // step 11
    if (!desc->configurable() && !isFixed) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_NE_AS_NC);
        return false;
    }

    // step 12
    rval.set(trapResult);
    return true;
}

// TrapDefineOwnProperty(O, P, DescObj, Throw)
static bool
TrapDefineOwnProperty(JSContext *cx, HandleObject proxy, HandleId id, MutableHandleValue vp)
{
    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step 3
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().defineProperty, &trap))
        return false;

    // step 4
    if (trap.isUndefined()) {
        AutoPropertyDescriptorRooter desc(cx);
        if (!ParsePropertyDescriptorObject(cx, proxy, vp, &desc))
            return false;
        return JS_DefinePropertyById(cx, target, id, desc.value, desc.getter, desc.setter,
                                     desc.attrs);
    }

    // step 5
    RootedValue normalizedDesc(cx, vp);
    if (!NormalizePropertyDescriptor(cx, &normalizedDesc))
        return false;

    // step 6
    RootedValue value(cx);
    if (!IdToValue(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectValue(*target),
        value,
        normalizedDesc
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 3, argv, trapResult.address()))
        return false;

    // steps 7-8
    if (ToBoolean(trapResult)) {
        bool isFixed;
        if (!HasOwn(cx, target, id, &isFixed))
            return false;

        if (!target->isExtensible() && !isFixed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_DEFINE_NEW);
            return false;
        }

        AutoPropDescArrayRooter descs(cx);
        PropDesc *desc = descs.append();
        if (!desc || !desc->initialize(cx, normalizedDesc))
            return false;

        if (isFixed) {
            bool valid;
            if (!ValidateProperty(cx, target, id, desc, &valid))
                return false;
            if (!valid) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_DEFINE_INVALID);
                return false;
            }
        }

        if (!desc->configurable() && !isFixed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_DEFINE_NE_AS_NC);
            return false;
        }

        vp.set(BooleanValue(true));
        return true;
    }

    // step 9
    // FIXME: API does not include a Throw parameter
    vp.set(BooleanValue(false));
    return true;
}

static inline void
ReportInvalidTrapResult(JSContext *cx, JSObject *proxy, JSAtom *atom)
{
    RootedValue v(cx, ObjectOrNullValue(proxy));
    JSAutoByteString bytes;
    if (!js_AtomToPrintableString(cx, atom, &bytes))
        return;
    js_ReportValueError2(cx, JSMSG_INVALID_TRAP_RESULT, JSDVG_IGNORE_STACK, v,
                         NullPtr(), bytes.ptr());
}

// This function is shared between getOwnPropertyNames, enumerate, and keys
static bool
ArrayToIdVector(JSContext *cx, HandleObject proxy, HandleObject target, HandleValue v,
                AutoIdVector &props, unsigned flags, JSAtom *trapName_)
{
    JS_ASSERT(v.isObject());
    RootedObject array(cx, &v.toObject());
    RootedAtom trapName(cx, trapName_);

    // steps g-h
    uint32_t n;
    if (!GetLengthProperty(cx, array, &n))
        return false;

    // steps i-k
    for (uint32_t i = 0; i < n; ++i) {
        // step i
        RootedValue v(cx);
        if (!JSObject::getElement(cx, array, array, i, &v))
            return false;

        // step ii
        RootedId id(cx);
        if (!ValueToId<CanGC>(cx, v, &id))
            return false;

        // step iii
        for (uint32_t j = 0; j < i; ++j) {
            if (props[j] == id) {
                ReportInvalidTrapResult(cx, proxy, trapName);
                return false;
            }
        }

        // step iv
        bool isFixed;
        if (!HasOwn(cx, target, id, &isFixed))
            return false;

        // step v
        if (!target->isExtensible() && !isFixed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_NEW);
            return false;
        }

        // step vi
        if (!props.append(id))
            return false;
    }

    // step l
    AutoIdVector ownProps(cx);
    if (!GetPropertyNames(cx, target, flags, &ownProps))
        return false;

    // step m
    for (size_t i = 0; i < ownProps.length(); ++i) {
        RootedId id(cx, ownProps[i]);

        bool found = false;
        for (size_t j = 0; j < props.length(); ++j) {
            if (props[j] == id) {
                found = true;
               break;
            }
        }
        if (found)
            continue;

        // step i
        bool sealed;
        if (!IsSealed(cx, target, id, &sealed))
            return false;
        if (sealed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_SKIP_NC);
            return false;
        }

        // step ii
        bool isFixed;
        if (!HasOwn(cx, target, id, &isFixed))
            return false;

        // step iii
        if (!target->isExtensible() && isFixed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_E_AS_NE);
            return false;
        }
    }

    // step n
    return true;
}

ScriptedDirectProxyHandler::ScriptedDirectProxyHandler()
        : DirectProxyHandler(&sScriptedDirectProxyHandlerFamily)
{
}

ScriptedDirectProxyHandler::~ScriptedDirectProxyHandler()
{
}

// FIXME: Move to Proxy::getPropertyDescriptor once ScriptedIndirectProxy is removed
bool
ScriptedDirectProxyHandler::getPropertyDescriptor(JSContext *cx, JSObject *proxy_, jsid id_,
                                                  PropertyDescriptor *desc, unsigned flags)
{
    JS_CHECK_RECURSION(cx, return false);
    Rooted<JSObject*> proxy(cx, proxy_);
    Rooted<jsid> id(cx, id_);
    if (!GetOwnPropertyDescriptor(cx, proxy, id, desc))
        return false;
    if (desc->obj)
        return true;
    RootedObject proto(cx);
    if (!JSObject::getProto(cx, proxy, &proto))
        return false;
    if (!proto) {
        JS_ASSERT(!desc->obj);
        return true;
    }
    return JS_GetPropertyDescriptorById(cx, proto, id, 0, desc);
}

bool
ScriptedDirectProxyHandler::getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy_, jsid id_,
                                                     PropertyDescriptor *desc, unsigned flags)
{
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);

    // step 1
    RootedValue v(cx);
    if (!TrapGetOwnProperty(cx, proxy, id, &v))
        return false;

    // step 2
    if (v.isUndefined()) {
        desc->obj = NULL;
        return true;
    }

    // steps 3-4
    return ParsePropertyDescriptorObject(cx, proxy, v, desc, true);
}

bool
ScriptedDirectProxyHandler::defineProperty(JSContext *cx, JSObject *proxy_, jsid id_,
                                           PropertyDescriptor *desc)
{
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);

    // step 1
    AutoPropDescArrayRooter descs(cx);
    PropDesc *d = descs.append();
    d->initFromPropertyDescriptor(*desc);
    RootedValue v(cx);
    if (!FromGenericPropertyDescriptor(cx, d, &v))
        return false;

    // step 2
    return TrapDefineOwnProperty(cx, proxy, id, &v);
}

bool
ScriptedDirectProxyHandler::getOwnPropertyNames(JSContext *cx, JSObject *proxy_,
                                                AutoIdVector &props)
{
    RootedObject proxy(cx, proxy_);

    // step a
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step b
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step c
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().getOwnPropertyNames, &trap))
        return false;

    // step d
    if (trap.isUndefined())
        return DirectProxyHandler::getOwnPropertyNames(cx, proxy, props);

    // step e
    Value argv[] = {
        ObjectValue(*target)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 1, argv, trapResult.address()))
        return false;

    // step f
    if (trapResult.isPrimitive()) {
        ReportInvalidTrapResult(cx, proxy, cx->names().getOwnPropertyNames);
        return false;
    }

    // steps g to n are shared
    return ArrayToIdVector(cx, proxy, target, trapResult, props, JSITER_OWNONLY | JSITER_HIDDEN,
                           cx->names().getOwnPropertyNames);
}

// Proxy.[[Delete]](P, Throw)
bool
ScriptedDirectProxyHandler::delete_(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);

    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step 3
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().deleteProperty, &trap))
        return false;

    // step 4
    if (trap.isUndefined())
        return DirectProxyHandler::delete_(cx, proxy, id, bp);

    // step 5
    RootedValue value(cx);
    if (!IdToValue(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectValue(*target),
        value
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 2, argv, trapResult.address()))
        return false;

    // step 6-7
    if (ToBoolean(trapResult)) {
        bool sealed;
        if (!IsSealed(cx, target, id, &sealed))
            return false;
        if (sealed) {
            RootedValue v(cx, IdToValue(id));
            js_ReportValueError(cx, JSMSG_CANT_DELETE, JSDVG_IGNORE_STACK, v, NullPtr());
            return false;
        }

        *bp = true;
        return true;
    }

    // step 8
    // FIXME: API does not include a Throw parameter
    *bp = false;
    return true;
}

// 12.6.4 The for-in Statement, step 6
bool
ScriptedDirectProxyHandler::enumerate(JSContext *cx, JSObject *proxy_, AutoIdVector &props)
{
    RootedObject proxy(cx, proxy_);

    // step a
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step b
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step c
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().enumerate, &trap))
        return false;

    // step d
    if (trap.isUndefined())
        return DirectProxyHandler::enumerate(cx, proxy, props);

    // step e
    Value argv[] = {
        ObjectOrNullValue(target)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 1, argv, trapResult.address()))
        return false;

    // step f
    if (trapResult.isPrimitive()) {
        JSAutoByteString bytes;
        if (!js_AtomToPrintableString(cx, cx->names().enumerate, &bytes))
            return false;
        RootedValue v(cx, ObjectOrNullValue(proxy));
        js_ReportValueError2(cx, JSMSG_INVALID_TRAP_RESULT, JSDVG_SEARCH_STACK,
                             v, NullPtr(), bytes.ptr());
        return false;
    }

    // steps g-m are shared
    // FIXME: the trap should return an iterator object, see bug 783826
    return ArrayToIdVector(cx, proxy, target, trapResult, props, 0, cx->names().enumerate);
}

// Proxy.[[HasProperty]](P)
bool
ScriptedDirectProxyHandler::has(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);

    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step 3
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().has, &trap))
        return false;

    // step 4
    if (trap.isUndefined())
        return DirectProxyHandler::has(cx, proxy, id, bp);

    // step 5
    RootedValue value(cx);
    if (!IdToValue(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectOrNullValue(target),
        value
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 2, argv, trapResult.address()))
        return false;

    // step 6
    bool success = ToBoolean(trapResult);;

    // step 7
    if (!success) {
        bool sealed;
        if (!IsSealed(cx, target, id, &sealed))
            return false;
        if (sealed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_NC_AS_NE);
            return false;
        }

        if (!target->isExtensible()) {
            bool isFixed;
            if (!HasOwn(cx, target, id, &isFixed))
                return false;
            if (isFixed) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_E_AS_NE);
                return false;
            }
        }
    }

    // step 8
    *bp = success;
    return true;
}

// Proxy.[[HasOwnProperty]](P)
bool
ScriptedDirectProxyHandler::hasOwn(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);

    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step 3
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().hasOwn, &trap))
        return false;

    // step 4
    if (trap.isUndefined())
        return DirectProxyHandler::hasOwn(cx, proxy, id, bp);

    // step 5
    RootedValue value(cx);
    if (!IdToValue(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectOrNullValue(target),
        value
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 2, argv, trapResult.address()))
        return false;

    // step 6
    bool success = ToBoolean(trapResult);

    // steps 7-8
    if (!success) {
        bool sealed;
        if (!IsSealed(cx, target, id, &sealed))
            return false;
        if (sealed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_NC_AS_NE);
            return false;
        }

        if (!target->isExtensible()) {
            bool isFixed;
            if (!HasOwn(cx, target, id, &isFixed))
                return false;
            if (isFixed) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_E_AS_NE);
                return false;
            }
        }
    } else if (!target->isExtensible()) {
        bool isFixed;
        if (!HasOwn(cx, target, id, &isFixed))
            return false;
        if (!isFixed) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_REPORT_NEW);
            return false;
        }
    }

    // step 9
    *bp = !!success;
    return true;
}

// Proxy.[[GetP]](P, Receiver)
bool
ScriptedDirectProxyHandler::get(JSContext *cx, JSObject *proxy_, JSObject *receiver_, jsid id_,
                                Value *vp)
{
    RootedObject proxy(cx, proxy_);
    RootedObject receiver(cx, receiver_);
    RootedId id(cx, id_);

    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step 3
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().get, &trap))
        return false;

    // step 4
    if (trap.isUndefined())
        return DirectProxyHandler::get(cx, proxy, receiver, id, vp);

    // step 5
    RootedValue value(cx);
    if (!IdToValue(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectOrNullValue(target),
        value,
        ObjectOrNullValue(receiver)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 3, argv, trapResult.address()))
        return false;

    // step 6
    AutoPropertyDescriptorRooter desc(cx);
    if (!GetOwnPropertyDescriptor(cx, target, id, &desc))
        return false;

    // step 7
    if (desc.obj) {
        if (IsDataDescriptor(desc) &&
            (desc.attrs & JSPROP_PERMANENT) &&
            (desc.attrs & JSPROP_READONLY))
        {
            bool same;
            if (!SameValue(cx, *vp, desc.value, &same))
                return false;
            if (!same) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MUST_REPORT_SAME_VALUE);
                return false;
            }
        }

        if (IsAccessorDescriptor(desc) &&
            (desc.attrs & JSPROP_PERMANENT) &&
            !(desc.attrs & JSPROP_GETTER))
        {
            if (!trapResult.isUndefined()) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MUST_REPORT_UNDEFINED);
                return false;
            }
        }
    }

    // step 8
    *vp = trapResult;
    return true;
}

// Proxy.[[SetP]](P, V, Receiver)
bool
ScriptedDirectProxyHandler::set(JSContext *cx, JSObject *proxy_, JSObject *receiver_, jsid id_,
                                bool strict, Value *vp)
{
    RootedObject proxy(cx, proxy_);
    RootedObject receiver(cx, receiver_);
    RootedId id(cx, id_);

    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step 3
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().set, &trap))
        return false;

    // step 4
    if (trap.isUndefined())
        return DirectProxyHandler::set(cx, proxy, receiver, id, strict, vp);

    // step 5
    RootedValue value(cx);
    if (!IdToValue(cx, id, &value))
        return false;
    Value argv[] = {
        ObjectOrNullValue(target),
        value,
        *vp,
        ObjectValue(*receiver)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 4, argv, trapResult.address()))
        return false;

    // step 6
    bool success = ToBoolean(trapResult);

    // step 7
    if (success) {
        AutoPropertyDescriptorRooter desc(cx);
        if (!GetOwnPropertyDescriptor(cx, target, id, &desc))
            return false;

        if (desc.obj) {
            if (IsDataDescriptor(desc) && (desc.attrs & JSPROP_PERMANENT) &&
                (desc.attrs & JSPROP_READONLY)) {
                bool same;
                if (!SameValue(cx, *vp, desc.value, &same))
                    return false;
                if (!same) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_SET_NW_NC);
                    return false;
                }
            }

            if (IsAccessorDescriptor(desc) && (desc.attrs & JSPROP_PERMANENT)) {
                if (!(desc.attrs & JSPROP_SETTER)) {
                    JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_CANT_SET_WO_SETTER);
                    return false;
                }
            }
        }
    }

    // step 8
    *vp = BooleanValue(success);
    return true;
}

// 15.2.3.14 Object.keys (O), step 2
bool
ScriptedDirectProxyHandler::keys(JSContext *cx, JSObject *proxy_, AutoIdVector &props)
{
    RootedObject proxy(cx, proxy_);

    // step a
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step b
    RootedObject target(cx, GetProxyTargetObject(proxy));

    // step c
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().keys, &trap))
        return false;

    // step d
    if (trap.isUndefined())
        return DirectProxyHandler::keys(cx, proxy, props);

    // step e
    Value argv[] = {
        ObjectOrNullValue(target)
    };
    RootedValue trapResult(cx);
    if (!Invoke(cx, ObjectValue(*handler), trap, 1, argv, trapResult.address()))
        return false;

    // step f
    if (trapResult.isPrimitive()) {
        JSAutoByteString bytes;
        if (!js_AtomToPrintableString(cx, cx->names().keys, &bytes))
            return false;
        RootedValue v(cx, ObjectOrNullValue(proxy));
        js_ReportValueError2(cx, JSMSG_INVALID_TRAP_RESULT, JSDVG_IGNORE_STACK,
                             v, NullPtr(), bytes.ptr());
        return false;
    }

    // steps g-n are shared
    return ArrayToIdVector(cx, proxy, target, trapResult, props, JSITER_OWNONLY, cx->names().keys);
}

bool
ScriptedDirectProxyHandler::iterate(JSContext *cx, JSObject *proxy, unsigned flags, Value *vp)
{
    // FIXME: Provide a proper implementation for this trap, see bug 787004
    return DirectProxyHandler::iterate(cx, proxy, flags, vp);
}

bool
ScriptedDirectProxyHandler::call(JSContext *cx, JSObject *proxy_, unsigned argc, Value *vp)
{
    RootedObject proxy(cx, proxy_);

    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    /*
     * NB: Remember to throw a TypeError here if we change NewProxyObject so that this trap can get
     * called for non-callable objects
     */

    // step 3
    RootedObject args(cx, NewDenseCopiedArray(cx, argc, vp + 2));
    if (!args)
        return false;

    // step 4
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().apply, &trap))
        return false;

    // step 5
    if (trap.isUndefined())
        return DirectProxyHandler::call(cx, proxy, argc, vp);

    // step 6
    Value argv[] = {
        ObjectValue(*target),
        vp[1],
        ObjectValue(*args)
    };
    RootedValue thisValue(cx, ObjectValue(*handler));
    return Invoke(cx, thisValue, trap, 3, argv, vp);
}

bool
ScriptedDirectProxyHandler::construct(JSContext *cx, JSObject *proxy_, unsigned argc, Value *argv,
                                      Value *rval)
{
    RootedObject proxy(cx, proxy_);

    // step 1
    RootedObject handler(cx, GetDirectProxyHandlerObject(proxy));

    // step 2
    RootedObject target(cx, GetProxyTargetObject(proxy));

    /*
     * NB: Remember to throw a TypeError here if we change NewProxyObject so that this trap can get
     * called for non-callable objects
     */

    // step 3
    RootedObject args(cx, NewDenseCopiedArray(cx, argc, argv));
    if (!args)
        return false;

    // step 4
    RootedValue trap(cx);
    if (!JSObject::getProperty(cx, handler, handler, cx->names().construct, &trap))
        return false;

    // step 5
    if (trap.isUndefined())
        return DirectProxyHandler::construct(cx, proxy, argc, argv, rval);

    // step 6
    Value constructArgv[] = {
        ObjectValue(*target),
        ObjectValue(*args)
    };
    RootedValue thisValue(cx, ObjectValue(*handler));
    return Invoke(cx, thisValue, trap, 2, constructArgv, rval);
}

ScriptedDirectProxyHandler ScriptedDirectProxyHandler::singleton;

#define INVOKE_ON_PROTOTYPE(cx, handler, proxy, protoCall)                   \
    JS_BEGIN_MACRO                                                           \
        RootedObject proto(cx);                                              \
        if (!handler->getPrototypeOf(cx, proxy, proto.address()))            \
            return false;                                                    \
        if (!proto)                                                          \
            return true;                                                     \
        assertSameCompartment(cx, proxy, proto);                             \
        return protoCall;                                                    \
    JS_END_MACRO                                                             \

bool
Proxy::getPropertyDescriptor(JSContext *cx, JSObject *proxy_, jsid id_, PropertyDescriptor *desc,
                             unsigned flags)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    desc->obj = NULL; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    if (!handler->hasPrototype())
        return handler->getPropertyDescriptor(cx, proxy, id, desc, flags);
    if (!handler->getOwnPropertyDescriptor(cx, proxy, id, desc, flags))
        return false;
    if (desc->obj)
        return true;
    INVOKE_ON_PROTOTYPE(cx, handler, proxy,
                        JS_GetPropertyDescriptorById(cx, proto, id, 0, desc));
}

bool
Proxy::getPropertyDescriptor(JSContext *cx, JSObject *proxy_, unsigned flags, jsid id, Value *vp)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    AutoPropertyDescriptorRooter desc(cx);
    if (!Proxy::getPropertyDescriptor(cx, proxy, id, &desc, flags))
        return false;

    RootedValue value(cx);
    if (!NewPropertyDescriptorObject(cx, &desc, &value))
        return false;
    *vp = value;
    return true;
}

bool
Proxy::getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy_, jsid id, PropertyDescriptor *desc,
                                unsigned flags)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    desc->obj = NULL; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->getOwnPropertyDescriptor(cx, proxy, id, desc, flags);
}

bool
Proxy::getOwnPropertyDescriptor(JSContext *cx, JSObject *proxy_, unsigned flags, jsid id,
                                Value *vp)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    AutoPropertyDescriptorRooter desc(cx);
    if (!Proxy::getOwnPropertyDescriptor(cx, proxy, id, &desc, flags))
        return false;

    RootedValue value(cx);
    if (!NewPropertyDescriptorObject(cx, &desc, &value))
        return false;
    *vp = value;
    return true;
}

bool
Proxy::defineProperty(JSContext *cx, JSObject *proxy_, jsid id, PropertyDescriptor *desc)
{
    JS_CHECK_RECURSION(cx, return false);
    BaseProxyHandler *handler = GetProxyHandler(proxy_);
    RootedObject proxy(cx, proxy_);
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return GetProxyHandler(proxy)->defineProperty(cx, proxy, id, desc);
}

bool
Proxy::defineProperty(JSContext *cx, JSObject *proxy_, jsid id_, const Value &v)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);
    AutoPropertyDescriptorRooter desc(cx);
    return ParsePropertyDescriptorObject(cx, proxy, v, &desc) &&
           Proxy::defineProperty(cx, proxy, id, &desc);
}

bool
Proxy::getOwnPropertyNames(JSContext *cx, JSObject *proxy_, AutoIdVector &props)
{
    JS_CHECK_RECURSION(cx, return false);
    BaseProxyHandler *handler = GetProxyHandler(proxy_);
    RootedObject proxy(cx, proxy_);
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return GetProxyHandler(proxy)->getOwnPropertyNames(cx, proxy, props);
}

bool
Proxy::delete_(JSContext *cx, JSObject *proxy_, jsid id, bool *bp)
{
    JS_CHECK_RECURSION(cx, return false);
    BaseProxyHandler *handler = GetProxyHandler(proxy_);
    RootedObject proxy(cx, proxy_);
    *bp = true; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return GetProxyHandler(proxy)->delete_(cx, proxy, id, bp);
}

JS_FRIEND_API(bool)
js::AppendUnique(JSContext *cx, AutoIdVector &base, AutoIdVector &others)
{
    AutoIdVector uniqueOthers(cx);
    if (!uniqueOthers.reserve(others.length()))
        return false;
    for (size_t i = 0; i < others.length(); ++i) {
        bool unique = true;
        for (size_t j = 0; j < base.length(); ++j) {
            if (others[i] == base[j]) {
                unique = false;
                break;
            }
        }
        if (unique)
            uniqueOthers.append(others[i]);
    }
    return base.append(uniqueOthers);
}

bool
Proxy::enumerate(JSContext *cx, JSObject *proxy_, AutoIdVector &props)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    if (!handler->hasPrototype())
        return GetProxyHandler(proxy)->enumerate(cx, proxy, props);
    if (!handler->keys(cx, proxy, props))
        return false;
    AutoIdVector protoProps(cx);
    INVOKE_ON_PROTOTYPE(cx, handler, proxy,
                        GetPropertyNames(cx, proto, 0, &protoProps) &&
                        AppendUnique(cx, props, protoProps));
}

bool
Proxy::has(JSContext *cx, JSObject *proxy_, jsid id_, bool *bp)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    RootedId id(cx, id_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    if (!handler->hasPrototype())
        return handler->has(cx, proxy, id, bp);
    if (!handler->hasOwn(cx, proxy, id, bp))
        return false;
    if (*bp)
        return true;
    JSBool Bp;
    INVOKE_ON_PROTOTYPE(cx, handler, proxy,
                        JS_HasPropertyById(cx, proto, id, &Bp) &&
                        ((*bp = Bp) || true));
}

bool
Proxy::hasOwn(JSContext *cx, JSObject *proxy_, jsid id, bool *bp)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->hasOwn(cx, proxy, id, bp);
}

bool
Proxy::get(JSContext *cx, HandleObject proxy, HandleObject receiver, HandleId id,
           MutableHandleValue vp)
{
    JS_CHECK_RECURSION(cx, return false);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    vp.setUndefined(); // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    bool own;
    if (!handler->hasPrototype()) {
        own = true;
    } else {
        if (!handler->hasOwn(cx, proxy, id, &own))
            return false;
    }
    if (own)
        return handler->get(cx, proxy, receiver, id, vp.address());
    INVOKE_ON_PROTOTYPE(cx, handler, proxy, JSObject::getGeneric(cx, proto, receiver, id, vp));
}

bool
Proxy::getElementIfPresent(JSContext *cx, HandleObject proxy, HandleObject receiver, uint32_t index,
                           MutableHandleValue vp, bool *present)
{
    JS_CHECK_RECURSION(cx, return false);

    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;

    BaseProxyHandler *handler = GetProxyHandler(proxy);
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();

    if (!handler->hasPrototype()) {
        return handler->getElementIfPresent(cx, proxy, receiver, index,
                                            vp.address(), present);
    }

    bool hasOwn;
    if (!handler->hasOwn(cx, proxy, id, &hasOwn))
        return false;

    if (hasOwn) {
        *present = true;
        return GetProxyHandler(proxy)->get(cx, proxy, receiver, id, vp.address());
    }

    INVOKE_ON_PROTOTYPE(cx, handler, proxy,
                        JSObject::getElementIfPresent(cx, proto, receiver, index, vp, present));
}

bool
Proxy::set(JSContext *cx, HandleObject proxy, HandleObject receiver, HandleId id, bool strict,
           MutableHandleValue vp)
{
    JS_CHECK_RECURSION(cx, return false);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    AutoEnterPolicy policy(cx, handler, proxy, id, BaseProxyHandler::SET, true);
    if (!policy.allowed())
        return policy.returnValue();
    if (handler->hasPrototype()) {
        // If we're using a prototype, we still want to use the proxy trap unless
        // we have a non-own property with a setter.
        bool hasOwn;
        if (!handler->hasOwn(cx, proxy, id, &hasOwn))
            return false;
        if (!hasOwn) {
            RootedObject proto(cx);
            if (!handler->getPrototypeOf(cx, proxy, proto.address()))
                return false;
            if (proto) {
                AutoPropertyDescriptorRooter desc(cx);
                if (!JS_GetPropertyDescriptorById(cx, proto, id, 0, &desc))
                    return false;
                if (desc.obj && desc.setter)
                    return JSObject::setGeneric(cx, proto, receiver, id, vp, strict);
            }
        }
    }
    return handler->set(cx, proxy, receiver, id, strict, vp.address());
}

bool
Proxy::keys(JSContext *cx, JSObject *proxy_, AutoIdVector &props)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return handler->keys(cx, proxy, props);
}

bool
Proxy::iterate(JSContext *cx, HandleObject proxy, unsigned flags, MutableHandleValue vp)
{
    JS_CHECK_RECURSION(cx, return false);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    vp.setUndefined(); // default result if we refuse to perform this action
    if (!handler->hasPrototype()) {
        AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID,
                               BaseProxyHandler::GET, true);
        // If the policy denies access but wants us to return true, we need
        // to hand a valid (empty) iterator object to the caller.
        if (!policy.allowed()) {
            AutoIdVector props(cx);
            return policy.returnValue() &&
                   EnumeratedIdVectorToIterator(cx, proxy, flags, props, vp);
        }
        return handler->iterate(cx, proxy, flags, vp.address());
    }
    AutoIdVector props(cx);
    // The other Proxy::foo methods do the prototype-aware work for us here.
    if ((flags & JSITER_OWNONLY)
        ? !Proxy::keys(cx, proxy, props)
        : !Proxy::enumerate(cx, proxy, props)) {
        return false;
    }
    return EnumeratedIdVectorToIterator(cx, proxy, flags, props, vp);
}

bool
Proxy::call(JSContext *cx, JSObject *proxy_, unsigned argc, Value *vp)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);

    // Because vp[0] is JS_CALLEE on the way in and JS_RVAL on the way out, we
    // can only set our default value once we're sure that we're not calling the
    // trap.
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID,
                           BaseProxyHandler::CALL, true);
    if (!policy.allowed()) {
        vp->setUndefined();
        return policy.returnValue();
    }

    return handler->call(cx, proxy, argc, vp);
}

bool
Proxy::construct(JSContext *cx, JSObject *proxy_, unsigned argc, Value *argv, Value *rval)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);

    // Because vp[0] is JS_CALLEE on the way in and JS_RVAL on the way out, we
    // can only set our default value once we're sure that we're not calling the
    // trap.
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID,
                           BaseProxyHandler::CALL, true);
    if (!policy.allowed()) {
        rval->setUndefined();
        return policy.returnValue();
    }

    return handler->construct(cx, proxy, argc, argv, rval);
}

bool
Proxy::nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl, CallArgs args)
{
    JS_CHECK_RECURSION(cx, return false);
    Rooted<JSObject*> proxy(cx, &args.thisv().toObject());
    // Note - we don't enter a policy here because our security architecture
    // guards against nativeCall by overriding the trap itself in the right
    // circumstances.
    return GetProxyHandler(proxy)->nativeCall(cx, test, impl, args);
}

bool
Proxy::hasInstance(JSContext *cx, HandleObject proxy, MutableHandleValue v, bool *bp)
{
    JS_CHECK_RECURSION(cx, return false);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    *bp = false; // default result if we refuse to perform this action
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID, BaseProxyHandler::GET, true);
    if (!policy.allowed())
        return policy.returnValue();
    return GetProxyHandler(proxy)->hasInstance(cx, proxy, v, bp);
}

bool
Proxy::objectClassIs(JSObject *proxy_, ESClassValue classValue, JSContext *cx)
{
    RootedObject proxy(cx, proxy_);
    return GetProxyHandler(proxy)->objectClassIs(proxy, classValue, cx);
}

JSString *
Proxy::obj_toString(JSContext *cx, JSObject *proxy_)
{
    JS_CHECK_RECURSION(cx, return NULL);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID,
                           BaseProxyHandler::GET, /* mayThrow = */ false);
    // Do the safe thing if the policy rejects.
    if (!policy.allowed()) {
        return handler->BaseProxyHandler::obj_toString(cx, proxy);
    }
    return handler->obj_toString(cx, proxy);
}

JSString *
Proxy::fun_toString(JSContext *cx, JSObject *proxy_, unsigned indent)
{
    JS_CHECK_RECURSION(cx, return NULL);
    RootedObject proxy(cx, proxy_);
    BaseProxyHandler *handler = GetProxyHandler(proxy);
    AutoEnterPolicy policy(cx, handler, proxy, JSID_VOID,
                           BaseProxyHandler::GET, /* mayThrow = */ false);
    // Do the safe thing if the policy rejects.
    if (!policy.allowed()) {
        if (proxy->isCallable())
            return JS_NewStringCopyZ(cx, "function () {\n    [native code]\n}");
        ReportIsNotFunction(cx, ObjectValue(*proxy));
        return NULL;
    }
    return handler->fun_toString(cx, proxy, indent);
}

bool
Proxy::regexp_toShared(JSContext *cx, JSObject *proxy_, RegExpGuard *g)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    return GetProxyHandler(proxy)->regexp_toShared(cx, proxy, g);
}

bool
Proxy::defaultValue(JSContext *cx, JSObject *proxy_, JSType hint, Value *vp)
{
    JS_CHECK_RECURSION(cx, return false);
    RootedObject proxy(cx, proxy_);
    return GetProxyHandler(proxy)->defaultValue(cx, proxy, hint, vp);
}

bool
Proxy::getPrototypeOf(JSContext *cx, JSObject *proxy, JSObject **proto)
{
    JS_CHECK_RECURSION(cx, return false);
    return GetProxyHandler(proxy)->getPrototypeOf(cx, proxy, proto);
}

JSObject * const Proxy::LazyProto = reinterpret_cast<JSObject *>(0x1);

static JSObject *
proxy_innerObject(JSContext *cx, HandleObject obj)
{
    return GetProxyPrivate(obj).toObjectOrNull();
}

static JSBool
proxy_LookupGeneric(JSContext *cx, HandleObject obj, HandleId id,
                    MutableHandleObject objp, MutableHandleShape propp)
{
    bool found;
    if (!Proxy::has(cx, obj, id, &found))
        return false;

    if (found) {
        MarkNonNativePropertyFound(propp);
        objp.set(obj);
    } else {
        objp.set(NULL);
        propp.set(NULL);
    }
    return true;
}

static JSBool
proxy_LookupProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                     MutableHandleObject objp, MutableHandleShape propp)
{
    Rooted<jsid> id(cx, NameToId(name));
    return proxy_LookupGeneric(cx, obj, id, objp, propp);
}

static JSBool
proxy_LookupElement(JSContext *cx, HandleObject obj, uint32_t index,
                    MutableHandleObject objp, MutableHandleShape propp)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return proxy_LookupGeneric(cx, obj, id, objp, propp);
}

static JSBool
proxy_LookupSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                    MutableHandleObject objp, MutableHandleShape propp)
{
    Rooted<jsid> id(cx, SPECIALID_TO_JSID(sid));
    return proxy_LookupGeneric(cx, obj, id, objp, propp);
}

static JSBool
proxy_DefineGeneric(JSContext *cx, HandleObject obj, HandleId id, HandleValue value,
                    PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    AutoPropertyDescriptorRooter desc(cx);
    desc.obj = obj;
    desc.value = value;
    desc.attrs = (attrs & (~JSPROP_SHORTID));
    desc.getter = getter;
    desc.setter = setter;
    desc.shortid = 0;
    return Proxy::defineProperty(cx, obj, id, &desc);
}

static JSBool
proxy_DefineProperty(JSContext *cx, HandleObject obj, HandlePropertyName name, HandleValue value,
                     PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    Rooted<jsid> id(cx, NameToId(name));
    return proxy_DefineGeneric(cx, obj, id, value, getter, setter, attrs);
}

static JSBool
proxy_DefineElement(JSContext *cx, HandleObject obj, uint32_t index, HandleValue value,
                    PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return proxy_DefineGeneric(cx, obj, id, value, getter, setter, attrs);
}

static JSBool
proxy_DefineSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid, HandleValue value,
                    PropertyOp getter, StrictPropertyOp setter, unsigned attrs)
{
    Rooted<jsid> id(cx, SPECIALID_TO_JSID(sid));
    return proxy_DefineGeneric(cx, obj, id, value, getter, setter, attrs);
}

static JSBool
proxy_GetGeneric(JSContext *cx, HandleObject obj, HandleObject receiver, HandleId id,
                 MutableHandleValue vp)
{
    return Proxy::get(cx, obj, receiver, id, vp);
}

static JSBool
proxy_GetProperty(JSContext *cx, HandleObject obj, HandleObject receiver, HandlePropertyName name,
                  MutableHandleValue vp)
{
    Rooted<jsid> id(cx, NameToId(name));
    return proxy_GetGeneric(cx, obj, receiver, id, vp);
}

static JSBool
proxy_GetElement(JSContext *cx, HandleObject obj, HandleObject receiver, uint32_t index,
                 MutableHandleValue vp)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return proxy_GetGeneric(cx, obj, receiver, id, vp);
}

static JSBool
proxy_GetElementIfPresent(JSContext *cx, HandleObject obj, HandleObject receiver, uint32_t index,
                          MutableHandleValue vp, bool *present)
{
    return Proxy::getElementIfPresent(cx, obj, receiver, index, vp, present);
}

static JSBool
proxy_GetSpecial(JSContext *cx, HandleObject obj, HandleObject receiver, HandleSpecialId sid,
                 MutableHandleValue vp)
{
    Rooted<jsid> id(cx, SPECIALID_TO_JSID(sid));
    return proxy_GetGeneric(cx, obj, receiver, id, vp);
}

static JSBool
proxy_SetGeneric(JSContext *cx, HandleObject obj, HandleId id,
                 MutableHandleValue vp, JSBool strict)
{
    return Proxy::set(cx, obj, obj, id, strict, vp);
}

static JSBool
proxy_SetProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                  MutableHandleValue vp, JSBool strict)
{
    Rooted<jsid> id(cx, NameToId(name));
    return proxy_SetGeneric(cx, obj, id, vp, strict);
}

static JSBool
proxy_SetElement(JSContext *cx, HandleObject obj, uint32_t index,
                 MutableHandleValue vp, JSBool strict)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return proxy_SetGeneric(cx, obj, id, vp, strict);
}

static JSBool
proxy_SetSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                 MutableHandleValue vp, JSBool strict)
{
    Rooted<jsid> id(cx, SPECIALID_TO_JSID(sid));
    return proxy_SetGeneric(cx, obj, id, vp, strict);
}

static JSBool
proxy_GetGenericAttributes(JSContext *cx, HandleObject obj, HandleId id, unsigned *attrsp)
{
    AutoPropertyDescriptorRooter desc(cx);
    if (!Proxy::getOwnPropertyDescriptor(cx, obj, id, &desc, 0))
        return false;
    *attrsp = desc.attrs;
    return true;
}

static JSBool
proxy_GetPropertyAttributes(JSContext *cx, HandleObject obj, HandlePropertyName name, unsigned *attrsp)
{
    Rooted<jsid> id(cx, NameToId(name));
    return proxy_GetGenericAttributes(cx, obj, id, attrsp);
}

static JSBool
proxy_GetElementAttributes(JSContext *cx, HandleObject obj, uint32_t index, unsigned *attrsp)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return proxy_GetGenericAttributes(cx, obj, id, attrsp);
}

static JSBool
proxy_GetSpecialAttributes(JSContext *cx, HandleObject obj, HandleSpecialId sid, unsigned *attrsp)
{
    Rooted<jsid> id(cx, SPECIALID_TO_JSID(sid));
    return proxy_GetGenericAttributes(cx, obj, id, attrsp);
}

static JSBool
proxy_SetGenericAttributes(JSContext *cx, HandleObject obj, HandleId id, unsigned *attrsp)
{
    /* Lookup the current property descriptor so we have setter/getter/value. */
    AutoPropertyDescriptorRooter desc(cx);
    if (!Proxy::getOwnPropertyDescriptor(cx, obj, id, &desc, JSRESOLVE_ASSIGNING))
        return false;
    desc.attrs = (*attrsp & (~JSPROP_SHORTID));
    return Proxy::defineProperty(cx, obj, id, &desc);
}

static JSBool
proxy_SetPropertyAttributes(JSContext *cx, HandleObject obj, HandlePropertyName name, unsigned *attrsp)
{
    Rooted<jsid> id(cx, NameToId(name));
    return proxy_SetGenericAttributes(cx, obj, id, attrsp);
}

static JSBool
proxy_SetElementAttributes(JSContext *cx, HandleObject obj, uint32_t index, unsigned *attrsp)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return proxy_SetGenericAttributes(cx, obj, id, attrsp);
}

static JSBool
proxy_SetSpecialAttributes(JSContext *cx, HandleObject obj, HandleSpecialId sid, unsigned *attrsp)
{
    Rooted<jsid> id(cx, SPECIALID_TO_JSID(sid));
    return proxy_SetGenericAttributes(cx, obj, id, attrsp);
}

static JSBool
proxy_DeleteGeneric(JSContext *cx, HandleObject obj, HandleId id,
                    MutableHandleValue rval, JSBool strict)
{
    // TODO: throwing away strict
    bool deleted;
    if (!Proxy::delete_(cx, obj, id, &deleted) || !js_SuppressDeletedProperty(cx, obj, id))
        return false;
    rval.setBoolean(deleted);
    return true;
}

static JSBool
proxy_DeleteProperty(JSContext *cx, HandleObject obj, HandlePropertyName name,
                     MutableHandleValue rval, JSBool strict)
{
    Rooted<jsid> id(cx, NameToId(name));
    return proxy_DeleteGeneric(cx, obj, id, rval, strict);
}

static JSBool
proxy_DeleteElement(JSContext *cx, HandleObject obj, uint32_t index,
                    MutableHandleValue rval, JSBool strict)
{
    RootedId id(cx);
    if (!IndexToId(cx, index, &id))
        return false;
    return proxy_DeleteGeneric(cx, obj, id, rval, strict);
}

static JSBool
proxy_DeleteSpecial(JSContext *cx, HandleObject obj, HandleSpecialId sid,
                    MutableHandleValue rval, JSBool strict)
{
    Rooted<jsid> id(cx, SPECIALID_TO_JSID(sid));
    return proxy_DeleteGeneric(cx, obj, id, rval, strict);
}

static void
proxy_TraceObject(JSTracer *trc, RawObject obj)
{
#ifdef DEBUG
    if (!trc->runtime->gcDisableStrictProxyCheckingCount && obj->isWrapper()) {
        JSObject *referent = &GetProxyPrivate(obj).toObject();
        if (referent->compartment() != obj->compartment()) {
            /*
             * Assert that this proxy is tracked in the wrapper map. We maintain
             * the invariant that the wrapped object is the key in the wrapper map.
             */
            Value key = ObjectValue(*referent);
            WrapperMap::Ptr p = obj->compartment()->lookupWrapper(key);
            JS_ASSERT(*p->value.unsafeGet() == ObjectValue(*obj));
        }
    }
#endif

    // NB: If you add new slots here, make sure to change
    // js::NukeChromeCrossCompartmentWrappers to cope.
    MarkCrossCompartmentSlot(trc, obj, &obj->getReservedSlotRef(JSSLOT_PROXY_PRIVATE), "private");
    MarkSlot(trc, &obj->getReservedSlotRef(JSSLOT_PROXY_EXTRA + 0), "extra0");

    /*
     * The GC can use the second reserved slot to link the cross compartment
     * wrappers into a linked list, in which case we don't want to trace it.
     */
    if (!IsCrossCompartmentWrapper(obj))
        MarkSlot(trc, &obj->getReservedSlotRef(JSSLOT_PROXY_EXTRA + 1), "extra1");
}

static void
proxy_TraceFunction(JSTracer *trc, RawObject obj)
{
    // NB: If you add new slots here, make sure to change
    // js::NukeChromeCrossCompartmentWrappers to cope.
    MarkCrossCompartmentSlot(trc, obj, &GetCall(obj), "call");
    MarkSlot(trc, &GetFunctionProxyConstruct(obj), "construct");
    proxy_TraceObject(trc, obj);
}

static JSObject *
proxy_WeakmapKeyDelegate(RawObject obj)
{
    JS_ASSERT(obj->isProxy());
    return GetProxyHandler(obj)->weakmapKeyDelegate(obj);
}

static JSBool
proxy_Convert(JSContext *cx, HandleObject proxy, JSType hint, MutableHandleValue vp)
{
    JS_ASSERT(proxy->isProxy());
    return Proxy::defaultValue(cx, proxy, hint, vp.address());
}

static void
proxy_Finalize(FreeOp *fop, RawObject obj)
{
    JS_ASSERT(obj->isProxy());
    GetProxyHandler(obj)->finalize(fop, obj);
}

static JSBool
proxy_HasInstance(JSContext *cx, HandleObject proxy, MutableHandleValue v, JSBool *bp)
{
    bool b;
    if (!Proxy::hasInstance(cx, proxy, v, &b))
        return false;
    *bp = !!b;
    return true;
}

#define PROXY_CLASS_EXT                             \
    {                                               \
        NULL,                /* outerObject */      \
        NULL,                /* innerObject */      \
        NULL,                /* iteratorObject */   \
        false,               /* isWrappedNative */  \
        proxy_WeakmapKeyDelegate                    \
    }

JS_FRIEND_DATA(Class) js::ObjectProxyClass = {
    "Proxy",
    Class::NON_NATIVE | JSCLASS_IMPLEMENTS_BARRIERS | JSCLASS_HAS_RESERVED_SLOTS(4),
    JS_PropertyStub,         /* addProperty */
    JS_PropertyStub,         /* delProperty */
    JS_PropertyStub,         /* getProperty */
    JS_StrictPropertyStub,   /* setProperty */
    JS_EnumerateStub,
    JS_ResolveStub,
    proxy_Convert,
    proxy_Finalize,          /* finalize    */
    NULL,                    /* checkAccess */
    NULL,                    /* call        */
    proxy_HasInstance,       /* hasInstance */
    NULL,                    /* construct   */
    proxy_TraceObject,       /* trace       */
    PROXY_CLASS_EXT,
    {
        proxy_LookupGeneric,
        proxy_LookupProperty,
        proxy_LookupElement,
        proxy_LookupSpecial,
        proxy_DefineGeneric,
        proxy_DefineProperty,
        proxy_DefineElement,
        proxy_DefineSpecial,
        proxy_GetGeneric,
        proxy_GetProperty,
        proxy_GetElement,
        proxy_GetElementIfPresent,
        proxy_GetSpecial,
        proxy_SetGeneric,
        proxy_SetProperty,
        proxy_SetElement,
        proxy_SetSpecial,
        proxy_GetGenericAttributes,
        proxy_GetPropertyAttributes,
        proxy_GetElementAttributes,
        proxy_GetSpecialAttributes,
        proxy_SetGenericAttributes,
        proxy_SetPropertyAttributes,
        proxy_SetElementAttributes,
        proxy_SetSpecialAttributes,
        proxy_DeleteProperty,
        proxy_DeleteElement,
        proxy_DeleteSpecial,
        NULL,                /* enumerate       */
        NULL,                /* thisObject      */
    }
};

JS_FRIEND_DATA(Class) js::OuterWindowProxyClass = {
    "Proxy",
    Class::NON_NATIVE | JSCLASS_IMPLEMENTS_BARRIERS | JSCLASS_HAS_RESERVED_SLOTS(4),
    JS_PropertyStub,         /* addProperty */
    JS_PropertyStub,         /* delProperty */
    JS_PropertyStub,         /* getProperty */
    JS_StrictPropertyStub,   /* setProperty */
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub,
    proxy_Finalize,          /* finalize    */
    NULL,                    /* checkAccess */
    NULL,                    /* call        */
    NULL,                    /* hasInstance */
    NULL,                    /* construct   */
    proxy_TraceObject,       /* trace       */
    {
        NULL,                /* outerObject */
        proxy_innerObject,
        NULL,                /* iteratorObject */
        false,               /* isWrappedNative */
        proxy_WeakmapKeyDelegate
    },
    {
        proxy_LookupGeneric,
        proxy_LookupProperty,
        proxy_LookupElement,
        proxy_LookupSpecial,
        proxy_DefineGeneric,
        proxy_DefineProperty,
        proxy_DefineElement,
        proxy_DefineSpecial,
        proxy_GetGeneric,
        proxy_GetProperty,
        proxy_GetElement,
        proxy_GetElementIfPresent,
        proxy_GetSpecial,
        proxy_SetGeneric,
        proxy_SetProperty,
        proxy_SetElement,
        proxy_SetSpecial,
        proxy_GetGenericAttributes,
        proxy_GetPropertyAttributes,
        proxy_GetElementAttributes,
        proxy_GetSpecialAttributes,
        proxy_SetGenericAttributes,
        proxy_SetPropertyAttributes,
        proxy_SetElementAttributes,
        proxy_SetSpecialAttributes,
        proxy_DeleteProperty,
        proxy_DeleteElement,
        proxy_DeleteSpecial,
        NULL,                /* enumerate       */
        NULL,                /* thisObject      */
    }
};

static JSBool
proxy_Call(JSContext *cx, unsigned argc, Value *vp)
{
    RootedObject proxy(cx, &JS_CALLEE(cx, vp).toObject());
    JS_ASSERT(proxy->isProxy());
    return Proxy::call(cx, proxy, argc, vp);
}

static JSBool
proxy_Construct(JSContext *cx, unsigned argc, Value *vp)
{
    RootedObject proxy(cx, &JS_CALLEE(cx, vp).toObject());
    JS_ASSERT(proxy->isProxy());
    return Proxy::construct(cx, proxy, argc, JS_ARGV(cx, vp), vp);
}

JS_FRIEND_DATA(Class) js::FunctionProxyClass = {
    "Proxy",
    Class::NON_NATIVE | JSCLASS_IMPLEMENTS_BARRIERS | JSCLASS_HAS_RESERVED_SLOTS(6),
    JS_PropertyStub,         /* addProperty */
    JS_PropertyStub,         /* delProperty */
    JS_PropertyStub,         /* getProperty */
    JS_StrictPropertyStub,   /* setProperty */
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub,
    proxy_Finalize,          /* finalize */
    NULL,                    /* checkAccess */
    proxy_Call,
    FunctionClass.hasInstance,
    proxy_Construct,
    proxy_TraceFunction,     /* trace       */
    PROXY_CLASS_EXT,
    {
        proxy_LookupGeneric,
        proxy_LookupProperty,
        proxy_LookupElement,
        proxy_LookupSpecial,
        proxy_DefineGeneric,
        proxy_DefineProperty,
        proxy_DefineElement,
        proxy_DefineSpecial,
        proxy_GetGeneric,
        proxy_GetProperty,
        proxy_GetElement,
        proxy_GetElementIfPresent,
        proxy_GetSpecial,
        proxy_SetGeneric,
        proxy_SetProperty,
        proxy_SetElement,
        proxy_SetSpecial,
        proxy_GetGenericAttributes,
        proxy_GetPropertyAttributes,
        proxy_GetElementAttributes,
        proxy_GetSpecialAttributes,
        proxy_SetGenericAttributes,
        proxy_SetPropertyAttributes,
        proxy_SetElementAttributes,
        proxy_SetSpecialAttributes,
        proxy_DeleteProperty,
        proxy_DeleteElement,
        proxy_DeleteSpecial,
        NULL,                /* enumerate       */
        NULL,                /* thisObject      */
    }
};

static JSObject *
NewProxyObject(JSContext *cx, BaseProxyHandler *handler, const Value &priv_, TaggedProto proto_,
                   JSObject *parent_, JSObject *call_, JSObject *construct_)
{
    RootedValue priv(cx, priv_);
    Rooted<TaggedProto> proto(cx, proto_);
    RootedObject parent(cx, parent_), call(cx, call_), construct(cx, construct_);

    JS_ASSERT_IF(proto.isObject(), cx->compartment == proto.toObject()->compartment());
    JS_ASSERT_IF(parent, cx->compartment == parent->compartment());
    JS_ASSERT_IF(construct, cx->compartment == construct->compartment());
    JS_ASSERT_IF(call && cx->compartment != call->compartment(), priv.get() == ObjectValue(*call));
    bool fun = call || construct;
    Class *clasp;
    if (fun)
        clasp = &FunctionProxyClass;
    else
        clasp = handler->isOuterWindow() ? &OuterWindowProxyClass : &ObjectProxyClass;

    /*
     * Eagerly mark properties unknown for proxies, so we don't try to track
     * their properties and so that we don't need to walk the compartment if
     * their prototype changes later.
     */
    if (proto.isObject()) {
        RootedObject protoObj(cx, proto.toObject());
        if (!JSObject::setNewTypeUnknown(cx, clasp, protoObj))
            return NULL;
    }

    NewObjectKind newKind = clasp == &OuterWindowProxyClass ? SingletonObject : GenericObject;
    gc::AllocKind allocKind = gc::GetGCObjectKind(clasp);
    if (handler->finalizeInBackground(priv))
        allocKind = GetBackgroundAllocKind(allocKind);
    RootedObject obj(cx, NewObjectWithGivenProto(cx, clasp, proto, parent, allocKind, newKind));
    if (!obj)
        return NULL;
    obj->initSlot(JSSLOT_PROXY_HANDLER, PrivateValue(handler));
    obj->initCrossCompartmentSlot(JSSLOT_PROXY_PRIVATE, priv);
    if (fun) {
        obj->initCrossCompartmentSlot(JSSLOT_PROXY_CALL, call ? ObjectValue(*call) : UndefinedValue());
        if (construct) {
            obj->initSlot(JSSLOT_PROXY_CONSTRUCT, ObjectValue(*construct));
        }
    }

    /* Don't track types of properties of proxies. */
    if (newKind != SingletonObject)
        MarkTypeObjectUnknownProperties(cx, obj->type());

    return obj;
}

JS_FRIEND_API(JSObject *)
js::NewProxyObject(JSContext *cx, BaseProxyHandler *handler, const Value &priv_, JSObject *proto_,
                   JSObject *parent_, JSObject *call_, JSObject *construct_)
{
    return NewProxyObject(cx, handler, priv_, TaggedProto(proto_), parent_, call_, construct_);
}

JSObject *
js::RenewProxyObject(JSContext *cx, JSObject *obj,
                     BaseProxyHandler *handler, Value priv)
{
    JS_ASSERT_IF(IsCrossCompartmentWrapper(obj), IsDeadProxyObject(obj));
    JS_ASSERT(obj->getParent() == cx->global());
    JS_ASSERT(obj->getClass() == &ObjectProxyClass);
    JS_ASSERT(obj->getTaggedProto().isLazy());
#ifdef DEBUG
    AutoSuppressGC suppressGC(cx);
    JS_ASSERT(!handler->isOuterWindow());
#endif

    obj->setSlot(JSSLOT_PROXY_HANDLER, PrivateValue(handler));
    obj->setCrossCompartmentSlot(JSSLOT_PROXY_PRIVATE, priv);
    obj->setSlot(JSSLOT_PROXY_EXTRA + 0, UndefinedValue());
    obj->setSlot(JSSLOT_PROXY_EXTRA + 1, UndefinedValue());

    return obj;
}

static JSBool
proxy(JSContext *cx, unsigned argc, jsval *vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 2) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "Proxy", "1", "s");
        return false;
    }
    RootedObject target(cx, NonNullObject(cx, args[0]));
    if (!target)
        return false;
    RootedObject handler(cx, NonNullObject(cx, args[1]));
    if (!handler)
        return false;
    RootedObject proto(cx);
    if (!JSObject::getProto(cx, target, &proto))
        return false;
    RootedObject fun(cx, target->isCallable() ? target : (JSObject *) NULL);
    JSObject *proxy = NewProxyObject(cx, &ScriptedDirectProxyHandler::singleton,
                                     ObjectValue(*target), proto, cx->global(),
                                     fun, fun);
    if (!proxy)
        return false;
    SetProxyExtra(proxy, 0, ObjectOrNullValue(handler));
    vp->setObject(*proxy);
    return true;
}

Class js::ProxyClass = {
    "Proxy",
    JSCLASS_HAS_CACHED_PROTO(JSProto_Proxy),
    JS_PropertyStub,         /* addProperty */
    JS_PropertyStub,         /* delProperty */
    JS_PropertyStub,         /* getProperty */
    JS_StrictPropertyStub,   /* setProperty */
    JS_EnumerateStub,
    JS_ResolveStub,
    JS_ConvertStub,
    NULL,                    /* finalize */
    NULL,                    /* checkAccess */
    NULL,                    /* call */
    NULL,                    /* hasInstance */
    proxy                    /* construct */
};

static JSBool
proxy_create(JSContext *cx, unsigned argc, Value *vp)
{
    if (argc < 1) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "create", "0", "s");
        return false;
    }
    JSObject *handler = NonNullObject(cx, vp[2]);
    if (!handler)
        return false;
    JSObject *proto, *parent = NULL;
    if (argc > 1 && vp[3].isObject()) {
        proto = &vp[3].toObject();
        parent = proto->getParent();
    } else {
        JS_ASSERT(IsFunctionObject(vp[0]));
        proto = NULL;
    }
    if (!parent)
        parent = vp[0].toObject().getParent();
    JSObject *proxy = NewProxyObject(cx, &ScriptedIndirectProxyHandler::singleton,
                                     ObjectValue(*handler), proto, parent);
    if (!proxy)
        return false;

    vp->setObject(*proxy);
    return true;
}

static JSBool
proxy_createFunction(JSContext *cx, unsigned argc, Value *vp)
{
    if (argc < 2) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "createFunction", "1", "");
        return false;
    }
    RootedObject handler(cx, NonNullObject(cx, vp[2]));
    if (!handler)
        return false;
    RootedObject proto(cx), parent(cx);
    parent = vp[0].toObject().getParent();
    proto = parent->global().getOrCreateFunctionPrototype(cx);
    if (!proto)
        return false;
    parent = proto->getParent();

    RootedObject call(cx, ValueToCallable(cx, vp[3], argc - 2));
    if (!call)
        return false;
    JSObject *construct = NULL;
    if (argc > 2) {
        construct = ValueToCallable(cx, vp[4], argc - 3);
        if (!construct)
            return false;
    }

    JSObject *proxy = NewProxyObject(cx, &ScriptedIndirectProxyHandler::singleton,
                                     ObjectValue(*handler), proto, parent, call, construct);
    if (!proxy)
        return false;

    vp->setObject(*proxy);
    return true;
}

static JSFunctionSpec static_methods[] = {
    JS_FN("create",         proxy_create,          2, 0),
    JS_FN("createFunction", proxy_createFunction,  3, 0),
    JS_FS_END
};

JS_FRIEND_API(JSObject *)
js_InitProxyClass(JSContext *cx, HandleObject obj)
{
    RootedObject module(cx, NewObjectWithClassProto(cx, &ProxyClass, NULL, obj, SingletonObject));
    if (!module)
        return NULL;

    if (!JS_DefineProperty(cx, obj, "Proxy", OBJECT_TO_JSVAL(module),
                           JS_PropertyStub, JS_StrictPropertyStub, 0)) {
        return NULL;
    }
    if (!JS_DefineFunctions(cx, module, static_methods))
        return NULL;

    MarkStandardClassInitializedNoProto(obj, &ProxyClass);

    return module;
}
