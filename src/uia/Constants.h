// define necessary constants here if UIAutomationClient.h is not available

#ifndef __UIA_PatternIds_MODULE_DEFINED__
const long UIA_TextPatternId = 10014;
const long UIA_ValuePatternId = 10002;
#endif //__UIA_PatternIds_MODULE_DEFINED__

#ifndef __UIA_EventIds_MODULE_DEFINED__
const long UIA_Text_TextSelectionChangedEventId = 20014;
#endif //__UIA_EventIds_MODULE_DEFINED__

#ifndef __UIA_PropertyIds_MODULE_DEFINED__
const long UIA_NamePropertyId = 30005;
const long UIA_ControlTypePropertyId = 30003;
const long UIA_IsKeyboardFocusablePropertyId = 30009;
const long UIA_AutomationIdPropertyId = 30011;
const long UIA_IsControlElementPropertyId = 30016;
const long UIA_IsContentElementPropertyId = 30017;
const long UIA_NativeWindowHandlePropertyId = 30020;
const long UIA_IsTextPatternAvailablePropertyId = 30040;
const long UIA_IsValuePatternAvailablePropertyId = 30043;
// headings are not a control type in UI Automation; the level is a property
// (UIAutomationClient.h: UIA_HeadingLevelPropertyId = 30173)
const long UIA_HeadingLevelPropertyId = 30173;
#endif //__UIA_PropertyIds_MODULE_DEFINED__

#ifndef __UIA_ValueIds_MODULE_DEFINED__
const long UIA_HeadingLevel_None = 80050;
const long UIA_HeadingLevel1 = 80051;
const long UIA_HeadingLevel2 = 80052;
const long UIA_HeadingLevel3 = 80053;
const long UIA_HeadingLevel4 = 80054;
const long UIA_HeadingLevel5 = 80055;
const long UIA_HeadingLevel6 = 80056;
#endif //__UIA_ValueIds_MODULE_DEFINED__

#ifndef __UIA_ControlTypeIds_MODULE_DEFINED__
const long UIA_CustomControlTypeId = 50025;
const long UIA_DocumentControlTypeId = 50030;
// a structural element of a tagged PDF: a run of text the document marks up as
// a heading, paragraph, list item, table cell and so on
const long UIA_GroupControlTypeId = 50026;
#endif //__UIA_ControlTypeIds_MODULE_DEFINED__
