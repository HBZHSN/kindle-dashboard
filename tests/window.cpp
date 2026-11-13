#include "preview.hpp"
#include <iostream>
#include <thread>

static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    try {
        dash::Preview preview(90, true);
        auto *display = XOpenDisplay(nullptr);
        check(display, "X11 connection");
        auto root = DefaultRootWindow(display);
        Window returned, parent, *children = nullptr, window = 0;
        unsigned count = 0;
        XQueryTree(display, root, &returned, &parent, &children, &count);
        for (unsigned i = 0; i < count; ++i) {
            char *name = nullptr;
            if (XFetchName(display, children[i], &name) && name) {
                if (std::string(name).find("Kindle Dashboard - Sample data") == 0)
                    window = children[i];
                XFree(name);
            }
        }
        if (children)
            XFree(children);
        check(window, "native window not found");
        auto pump = [&] {
            XSync(display, False);
            for (int i = 0; i < 5; ++i) {
                preview.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        };
        dash::Image frame;
        for (int y = 0; y < frame.height; ++y)
            for (int x = 0; x < frame.width / 2; ++x)
                frame.pixel(x, y, 0);
        XResizeWindow(display, window, 720, 540);
        preview.show(frame);
        pump();
        auto pixel = [&](int x, int y) {
            auto *image = XGetImage(display, window, x, y, 1, 1, AllPlanes, ZPixmap);
            check(image, "window capture failed");
            auto result = XGetPixel(image, 0, 0);
            XDestroyImage(image);
            return result;
        };
        check(pixel(180, 270) == BlackPixel(display, DefaultScreen(display)), "left half mismatch");
        check(pixel(540, 270) == WhitePixel(display, DefaultScreen(display)), "right half mismatch");
        XEvent click{};
        click.xbutton.type = ButtonRelease;
        click.xbutton.display = display;
        click.xbutton.window = window;
        click.xbutton.x = 540;
        click.xbutton.y = 270;
        XSendEvent(display, window, False, ButtonReleaseMask, &click);
        pump();
        auto taps = preview.take_taps();
        check(taps.size() == 1 && taps[0].first >= 600, "right-side mouse click not mapped");
        frame = dash::Image();
        preview.show(frame);
        pump();
        check(pixel(180, 270) == WhitePixel(display, DefaultScreen(display)), "updated frame not displayed");
        XEvent key{};
        key.xkey.type = KeyPress;
        key.xkey.display = display;
        key.xkey.window = window;
        key.xkey.keycode = XKeysymToKeycode(display, XK_r);
        XSendEvent(display, window, False, KeyPressMask, &key);
        pump();
        check(pixel(10, 270) != WhitePixel(display, DefaultScreen(display)), "rotation did not letterbox");
        check(pixel(360, 270) == WhitePixel(display, DefaultScreen(display)), "rotated frame missing");
        XEvent close{};
        close.xclient.type = ClientMessage;
        close.xclient.window = window;
        close.xclient.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
        close.xclient.format = 32;
        close.xclient.data.l[0] = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSendEvent(display, window, False, NoEventMask, &close);
        pump();
        check(dash::stopping, "closing the window did not stop the application");
        XCloseDisplay(display);
        std::cout << "PASS: native WSLg window pixels, resize, frame update, rotation, close\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
