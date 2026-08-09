/*
 * GraphicsConfiguration for the AmigaOS 4 toolkit (Phase 4 M4).
 * The Workbench screen presented as a 32-bit direct-color raster.
 * GPLv2+Classpath-exception (java-os4 project).
 */
package sun.awt.amiga;

import java.awt.GraphicsConfiguration;
import java.awt.GraphicsDevice;
import java.awt.Rectangle;
import java.awt.Transparency;
import java.awt.geom.AffineTransform;
import java.awt.image.ColorModel;
import java.awt.image.DirectColorModel;

public final class AmigaGraphicsConfig extends GraphicsConfiguration {

    private final AmigaGraphicsDevice device;
    private static Rectangle screenBounds;

    AmigaGraphicsConfig(AmigaGraphicsDevice device) {
        this.device = device;
    }

    /*
     * The Workbench screen's size, asked again until it can actually be
     * answered.
     *
     * The failed answer used to be cached like a real one, and that is worse
     * than it sounds.  screensize0() returns 0 when it cannot lock the public
     * screen -- most plausibly when it is called early, before Intuition is up
     * -- and the first caller wins the cache for the life of the VM.  So a
     * single early miss pinned every later question to the 1280x800 default,
     * with nothing said anywhere.
     *
     * On a screen that is not 1280x800 the whole layer above then works from a
     * wrong number, and the two symptoms of that are exactly what was reported:
     * windows sized against the assumed screen come out LARGER than the real
     * one, and setLocationRelativeTo centring a window wider than the screen it
     * is centring on computes (screen - window) / 2, which is NEGATIVE.
     *
     * So: cache only a real reading, retry otherwise, and say so once. If the
     * complaint survives this, the default was not the cause and the message
     * will not appear.
     */
    private static boolean warnedAboutDefault;

    static synchronized Rectangle getScreenBounds() {
        if (screenBounds != null)
            return screenBounds;

        int wh = 0;
        try {
            wh = AmigaNative.screensize0();
        } catch (Throwable t) {
            /* fall through to the default */
        }

        if (wh != 0) {
            screenBounds = new Rectangle(0, 0, (wh >> 16) & 0xFFFF, wh & 0xFFFF);
            return screenBounds;
        }

        if (!warnedAboutDefault) {
            warnedAboutDefault = true;
            System.err.println("[AWT] screen size unavailable (screensize0 "
                + "returned 0); assuming 1280x800 until it can be read. "
                + "Windows may be laid out for the wrong screen.");
        }
        /* Deliberately NOT stored: the next caller asks again. */
        return new Rectangle(0, 0, 1280, 800);
    }

    @Override
    public GraphicsDevice getDevice() {
        return device;
    }

    @Override
    public ColorModel getColorModel() {
        return new DirectColorModel(24, 0x00ff0000, 0x0000ff00, 0x000000ff);
    }

    @Override
    public ColorModel getColorModel(int transparency) {
        switch (transparency) {
            case Transparency.OPAQUE:
                return getColorModel();
            case Transparency.BITMASK:
            case Transparency.TRANSLUCENT:
                return ColorModel.getRGBdefault();
            default:
                return null;
        }
    }

    @Override
    public AffineTransform getDefaultTransform() {
        return new AffineTransform();
    }

    @Override
    public AffineTransform getNormalizingTransform() {
        return new AffineTransform();
    }

    @Override
    public Rectangle getBounds() {
        return new Rectangle(getScreenBounds());
    }
}
