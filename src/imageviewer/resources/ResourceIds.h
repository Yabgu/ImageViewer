// Resource IDs shared by the .rc file and the Win32 code that loads them.
//
// iview.rc embeds the application icon with this ID; Window.ixx loads the small
// icon out of the same group so the title bar uses the real 16 px artwork
// instead of a downscaled 32 px bitmap.  Keeping the number here means the two
// sides cannot drift apart.

#pragma once

#define IVIEW_ICON_ID 1
