/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the Mozilla OS/2 libraries.
 *
 * The Initial Developer of the Original Code is John Fairhurst,
 * <john_fairhurst@iname.com>.  Portions created by John Fairhurst are
 * Copyright (C) 1999 John Fairhurst. All Rights Reserved.
 *
 * Contributor(s): 
 *
 */

// Push button control; don't really come any simpler than this...

#include "nsPushButton.h"

// XP-com
NS_IMPL_ADDREF(nsPushButton)
NS_IMPL_RELEASE(nsPushButton)

nsresult nsPushButton::QueryInterface( const nsIID &aIID, void **aInstancePtr)
{
  nsresult result = nsWindow::QueryInterface( aIID, aInstancePtr);

  if( result == NS_NOINTERFACE && aIID.Equals( nsIButton::GetIID()))
  {
     *aInstancePtr = (void*) ((nsIButton*)this);
     NS_ADDREF_THIS();
     result = NS_OK;
  }

  return result;
}

// Text
NS_IMPL_LABEL(nsPushButton)

// Creation hooks
PCSZ nsPushButton::WindowClass()
{
  return WC_BUTTON;
}

ULONG nsPushButton::WindowStyle()
{ 
   return BASE_CONTROL_STYLE | BS_PUSHBUTTON;
}
