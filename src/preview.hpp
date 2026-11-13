#pragma once
#include "dashboard.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <cstdlib>
#include <stdexcept>

namespace dash {
// Development-only window. It displays the same framebuffer passed to FBInk.
class Preview {
    ::Display *display_ = nullptr;
    Window window_ = 0;
    Atom close_ = 0;
    GC gc_ = nullptr;
    Image frame_;
    int width_ = 1080, height_ = 810, rotation_;
    bool physical_ = false, dirty_ = true, have_frame_ = false;
    std::vector<std::pair<int, int>> taps_;

    std::pair<int, int> logical_point(int x, int y) const {
        int sw = physical_ ? 1072 : WIDTH, sh = physical_ ? 1448 : HEIGHT;
        double scale = std::min(double(width_) / sw, double(height_) / sh);
        int w = std::max(1, int(sw * scale)), h = std::max(1, int(sh * scale));
        int left = (width_ - w) / 2, top = (height_ - h) / 2;
        if (x < left || x >= left + w || y < top || y >= top + h)
            return {-1, -1};
        int px = (x - left) * sw / w, py = (y - top) * sh / h;
        if (!physical_)
            return {px, py};
        return rotation_ == 90 ? std::pair<int, int>{std::clamp(py - 4, 0, WIDTH - 1),
                                                     std::clamp(1075 - px, 0, HEIGHT - 1)}
                               : std::pair<int, int>{std::clamp(1443 - py, 0, WIDTH - 1),
                                                     std::clamp(px + 4, 0, HEIGHT - 1)};
    }

    void draw() {
        if (!dirty_ || !have_frame_ || !window_)
            return;
        auto physical = physical_ ? device_frame(frame_, rotation_) : Image(1, 1);
        const auto &source = physical_ ? physical : frame_;
        auto screen = DefaultScreen(display_);
        auto visual = DefaultVisual(display_, screen);
        XImage *image = XCreateImage(display_, visual, DefaultDepth(display_, screen), ZPixmap, 0, nullptr,
                                     width_, height_, 32, 0);
        if (!image)
            throw std::runtime_error("Cannot allocate X11 image");
        image->data = static_cast<char *>(std::calloc(image->bytes_per_line, image->height));
        if (!image->data) {
            XDestroyImage(image);
            throw std::bad_alloc();
        }
        unsigned long colors[256];
        auto channel = [](unsigned value, unsigned long mask) {
            if (!mask)
                return 0UL;
            unsigned shift = 0;
            while (!(mask & 1)) {
                ++shift;
                mask >>= 1;
            }
            return ((value * mask + 127) / 255) << shift;
        };
        for (unsigned i = 0; i < 256; ++i)
            colors[i] =
                channel(i, visual->red_mask) | channel(i, visual->green_mask) | channel(i, visual->blue_mask);
        double scale = std::min(double(width_) / source.width, double(height_) / source.height);
        int w = std::max(1, int(source.width * scale)), h = std::max(1, int(source.height * scale));
        int left = (width_ - w) / 2, top = (height_ - h) / 2;
        for (int y = 0; y < height_; ++y)
            for (int x = 0; x < width_; ++x) {
                uint8_t gray = 215;
                if (x >= left && x < left + w && y >= top && y < top + h)
                    gray = source.pixels[size_t((y - top) * source.height / h) * source.width +
                                         (x - left) * source.width / w];
                XPutPixel(image, x, y, colors[gray]);
            }
        XPutImage(display_, window_, gc_, image, 0, 0, 0, 0, width_, height_);
        XDestroyImage(image);
        XFlush(display_);
        dirty_ = false;
    }

  public:
    Preview(int rotation, bool offline) : rotation_(rotation) {
        display_ = XOpenDisplay(nullptr);
        if (!display_)
            throw std::runtime_error("Cannot open WSLg display; check DISPLAY and /mnt/wslg");
        int screen = DefaultScreen(display_);
        width_ = std::min(1080, DisplayWidth(display_, screen) * 4 / 5);
        height_ = std::min(810, DisplayHeight(display_, screen) * 4 / 5);
        window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen), 0, 0, width_, height_, 0, 0,
                                      WhitePixel(display_, screen));
        XStoreName(display_, window_,
                   offline ? "Kindle Dashboard - Sample data | R: rotate | Esc: close"
                           : "Kindle Dashboard - Live data | R: rotate | Esc: close");
        XSelectInput(display_, window_, ExposureMask | StructureNotifyMask | KeyPressMask | ButtonReleaseMask);
        close_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &close_, 1);
        gc_ = XCreateGC(display_, window_, 0, nullptr);
        XMapWindow(display_, window_);
        XFlush(display_);
    }
    ~Preview() {
        if (gc_)
            XFreeGC(display_, gc_);
        if (window_)
            XDestroyWindow(display_, window_);
        if (display_)
            XCloseDisplay(display_);
    }
    Preview(const Preview &) = delete;
    Preview &operator=(const Preview &) = delete;
    void show(const Image &frame) {
        frame_ = frame;
        have_frame_ = dirty_ = true;
        poll();
    }
    void poll() {
        while (XPending(display_)) {
            XEvent event;
            XNextEvent(display_, &event);
            if (event.type == ClientMessage && Atom(event.xclient.data.l[0]) == close_)
                stopping = true;
            else if (event.type == DestroyNotify) {
                window_ = 0;
                stopping = true;
            } else if (event.type == Expose)
                dirty_ = true;
            else if (event.type == ConfigureNotify) {
                width_ = std::max(1, event.xconfigure.width);
                height_ = std::max(1, event.xconfigure.height);
                dirty_ = true;
            } else if (event.type == KeyPress) {
                auto key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape || key == XK_q)
                    stopping = true;
                if (key == XK_r || key == XK_R) {
                    physical_ = !physical_;
                    dirty_ = true;
                }
            } else if (event.type == ButtonRelease) {
                auto point = logical_point(event.xbutton.x, event.xbutton.y);
                if (point.first >= 0)
                    taps_.push_back(point);
            }
        }
        if (!stopping)
            draw();
    }
    std::vector<std::pair<int, int>> take_taps() {
        std::vector<std::pair<int, int>> result;
        result.swap(taps_);
        return result;
    }
};
} // namespace dash
