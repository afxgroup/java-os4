/*
 * AmigaMouseInfoPeer -- java.awt.MouseInfo on AmigaOS.
 *
 * getMouseInfoPeer() used to throw:
 *
 *     java.lang.UnsupportedOperationException: MouseInfo is not supported on
 *     AmigaOS (Swing-only toolkit)
 *
 * and the caller was not application code asking for the pointer.  It was
 * FlatLaf positioning a tooltip -- FlatPopupFactory.fixToolTipLocation ->
 * hasTipLocation -> MouseInfo.getPointerInfo() -- so every tooltip threw on the
 * event dispatch thread.  A look-and-feel doing something ordinary is exactly
 * the traffic this toolkit has to carry, and "Swing-only" was never a reason
 * here: the pointer belongs to Intuition, not to a widget set, and the screen
 * knows where it is whether or not any native widgets exist.
 *
 * GPLv2 (java-os4 project).
 */
package sun.awt.amiga;

import java.awt.Point;
import java.awt.Window;
import java.awt.peer.MouseInfoPeer;

public final class AmigaMouseInfoPeer implements MouseInfoPeer {

    /*
     * Fills in the pointer position and returns which screen it is on.
     *
     * Always 0: java.awt.MouseInfo takes this as an index into
     * GraphicsEnvironment.getScreenDevices(), and this toolkit reports one
     * device (the Workbench screen).  Returning anything else would index past
     * the end of that array.
     */
    @Override
    public int fillPointWithCoords(Point point) {
        long packed = AmigaNative.mousepos0();

        point.x = (int) (packed >> 32);
        point.y = (int) packed;
        return 0;
    }

    /*
     * Whether the pointer is over this window.
     *
     * Answered from the window's own bounds rather than by asking Intuition
     * which window is under the pointer, and the difference matters: Intuition
     * would name the window that would RECEIVE the click, so a window covered
     * by another would answer false. Swing asks this to decide where a popup
     * may go, and treats the frame it is placing a tooltip for as a container,
     * not as the click target -- the geometric answer is the one it means.
     *
     * A window that is not showing is never under the pointer, whatever its
     * bounds say.
     */
    @Override
    public boolean isWindowUnderMouse(Window w) {
        if (w == null || !w.isShowing()) {
            return false;
        }

        long packed = AmigaNative.mousepos0();
        int x = (int) (packed >> 32);
        int y = (int) packed;

        Point origin = w.getLocationOnScreen();
        return x >= origin.x && x < origin.x + w.getWidth()
            && y >= origin.y && y < origin.y + w.getHeight();
    }
}
