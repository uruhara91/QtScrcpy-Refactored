#ifndef WAYLANDMOUSETAP_H
#define WAYLANDMOUSETAP_H

#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QWaylandClientExtension>
#include <memory>

#include "qwayland-pointer-constraints-unstable-v1.h"
#include "qwayland-relative-pointer-unstable-v1.h"

class QWindow;
struct wl_seat;

// Binds zwp_relative_pointer_manager_v1, the Wayland global that hands out
// per-pointer relative-motion objects. This is the "manager" side: on its
// own it does nothing but exist until a WaylandRelativePointer is requested
// from it (see WaylandMouseTap::enable()).
//
// QWaylandClientExtensionTemplate handles the wl_registry bind entirely
// internally (it listens for the compositor's registry advertisement and
// binds automatically) - this class only needs to exist and be initialized
// for isActive() to eventually become true once the compositor responds.
class WaylandRelativePointerManager
    : public QWaylandClientExtensionTemplate<WaylandRelativePointerManager>,
      public QtWayland::zwp_relative_pointer_manager_v1
{
    Q_OBJECT
public:
    WaylandRelativePointerManager();
};

// Binds zwp_pointer_constraints_v1, the Wayland global used to request a
// pointer lock (see WaylandMouseTap::enable()).
class WaylandPointerConstraints
    : public QWaylandClientExtensionTemplate<WaylandPointerConstraints>,
      public QtWayland::zwp_pointer_constraints_v1
{
    Q_OBJECT
public:
    WaylandPointerConstraints();
};

// A single active relative-pointer subscription (zwp_relative_pointer_v1).
// Emits rawMotion() for every relative_motion event the compositor sends -
// this is the unaccelerated, compositor-provided delta that
// InputConvertGame::relativeMouseMoveEvent() is fed from (see
// device->relativeMouseMoveEvent() call site in videoform.cpp).
class WaylandRelativePointer : public QObject,
                                public QtWayland::zwp_relative_pointer_v1
{
    Q_OBJECT
public:
    explicit WaylandRelativePointer(struct ::zwp_relative_pointer_v1 *object, QObject *parent = nullptr);
    ~WaylandRelativePointer() override;

signals:
    // dx/dy are in surface-local pixel units (already wl_fixed_t -> double
    // converted), unaccelerated. utime is the compositor timestamp in
    // microseconds (utime_hi/utime_lo combined) - exposed in case
    // finer-grained pacing is ever needed, not currently consumed.
    void rawMotion(QPointF delta, quint64 utimeUs);

protected:
    void zwp_relative_pointer_v1_relative_motion(uint32_t utime_hi, uint32_t utime_lo,
                                                  wl_fixed_t dx, wl_fixed_t dy,
                                                  wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) override;
};

// A single active pointer lock (zwp_locked_pointer_v1). Tracks the
// compositor's locked/unlocked confirmation; destroying this object (or
// calling destroy()) releases the lock.
class WaylandLockedPointer : public QObject,
                              public QtWayland::zwp_locked_pointer_v1
{
    Q_OBJECT
public:
    explicit WaylandLockedPointer(struct ::zwp_locked_pointer_v1 *object, QObject *parent = nullptr);
    ~WaylandLockedPointer() override;

signals:
    void locked();
    void unlocked();

protected:
    void zwp_locked_pointer_v1_locked() override;
    void zwp_locked_pointer_v1_unlocked() override;
};

// Top-level facade used by VideoForm. Owns the two protocol managers,
// (re)acquires the wl_surface/wl_pointer/wl_seat native handles as needed
// (they are not stable across window show/hide - see the comment on
// resolveNativeHandles()), and exposes a simple enable(bool)/rawMotion()
// API that mirrors the shape of the existing MouseTap interface closely
// enough to slot into VideoForm's grabCursor() handling, without forcing
// the two unrelated mechanisms (warp-based confine vs. compositor-native
// lock) into a single artificial base class.
//
// Every method here assumes it is only ever called when the platform is
// confirmed to be native Wayland (QGuiApplication::platformName() ==
// "wayland") - VideoForm is responsible for that check before constructing
// this class at all; see videoform.cpp.
class WaylandMouseTap : public QObject
{
    Q_OBJECT
public:
    explicit WaylandMouseTap(QWindow *window, QObject *parent = nullptr);
    ~WaylandMouseTap() override;

    // True once both Wayland globals (relative-pointer manager, pointer
    // constraints) have been bound by the compositor. Until this is true,
    // enable(true) will not attempt a lock - the caller (VideoForm) should
    // keep its existing warp-cursor fallback active in the meantime, and
    // can re-check this after protocolsReadyChanged() fires.
    [[nodiscard]] bool protocolsReady() const;

    // Requests (enabled=true) or releases (enabled=false) the pointer lock
    // + relative motion subscription. Safe to call repeatedly / redundantly
    // (e.g. enable(true) while already enabled is a no-op). Returns false
    // if the lock could not even be requested (protocols not ready, or no
    // wl_surface/wl_pointer available for the window right now) - the
    // caller should treat this the same as "compositor doesn't support it"
    // and fall back to warp-cursor confinement.
    bool enable(bool enabled);

    [[nodiscard]] bool isLocked() const { return m_lockedPointer != nullptr; }

    // Scale applied to every raw motion delta before it's forwarded via
    // rawMotion(). Needed because dx_unaccel/dy_unaccel (see
    // WaylandRelativePointer) are raw device-units, deliberately bypassing
    // the compositor's pointer-acceleration curve entirely (see the
    // rationale in zwp_relative_pointer_v1_relative_motion() in the .cpp) -
    // that curve is what made the old warp-cursor path's already-scaled
    // QMouseEvent deltas feel reasonable at the existing speedRatio keymap
    // setting. Without any compensation, unaccelerated device-units read
    // as dramatically more sensitive ("licin"/slippery) at the same
    // speedRatio. This is intentionally a *separate* knob from
    // InputConvertGame's speedRatio (sourced from the keymap JSON) rather
    // than folded into it, because speedRatio is shared with the
    // warp-cursor path (X11/Windows/macOS) which does NOT need this
    // compensation - changing speedRatio to fix Wayland feel would throw
    // off every other platform.
    //
    // 1000dpi is libinput's own normalization reference point for
    // accelerated coordinates (see "Normalization of relative motion",
    // wayland freedesktop libinput docs) - i.e. at 1000dpi, one raw
    // device-unit is roughly comparable to what one pixel felt like at low
    // speed under the old accelerated path. Higher-DPI mice will
    // correspondingly feel more sensitive than the old path at this
    // default; there is no single scale that is exactly equivalent for
    // every mouse, since the old path's feel was itself DPI-dependent
    // through the OS curve. Tune per-device via setSensitivityScale() if
    // needed.
    void setSensitivityScale(qreal scale) { m_sensitivityScale = scale; }
    [[nodiscard]] qreal sensitivityScale() const { return m_sensitivityScale; }

signals:
    // Mirrors WaylandRelativePointer::rawMotion(); forwarded here so
    // VideoForm only needs to know about WaylandMouseTap, not the
    // lower-level protocol wrapper objects.
    void rawMotion(QPointF delta);
    void protocolsReadyChanged(bool ready);
    // Emitted when the compositor confirms/revokes the lock. A revoked
    // lock (unlocked() with enable() never having been called with false)
    // can happen e.g. if the surface loses focus - the caller should not
    // assume the lock persists indefinitely once granted.
    void lockStateChanged(bool locked);

private slots:
    void onManagerActiveChanged();
    void onLockedPointerLocked();
    void onLockedPointerUnlocked();

private:
    // wl_surface handles are not stable for the lifetime of a QWindow: Qt
    // destroys and recreates the underlying Wayland surface across
    // hide/show cycles (this is documented compositor-integration
    // behavior, not a bug - see e.g. KWin's historical writeups on
    // QPlatformSurfaceEvent timing). This re-resolves the surface/pointer
    // handles from the platform native interface every time a lock is
    // requested, rather than caching them at construction time.
    struct NativeHandles {
        struct ::wl_surface *surface = nullptr;
        struct ::wl_pointer *pointer = nullptr;
    };
    [[nodiscard]] NativeHandles resolveNativeHandles() const;

    void teardownLock();

    QPointer<QWindow> m_window;
    std::unique_ptr<WaylandRelativePointerManager> m_relativePointerManager;
    std::unique_ptr<WaylandPointerConstraints> m_pointerConstraints;
    std::unique_ptr<WaylandRelativePointer> m_relativePointer;
    std::unique_ptr<WaylandLockedPointer> m_lockedPointer;
    bool m_wantEnabled = false;
    bool m_lastProtocolsReady = false;
    // Conservative default: most mice are >=1000dpi nowadays, at which the
    // old accelerated path's low-speed behavior was already close to
    // 1 device-unit ~= 1 pixel (see setSensitivityScale() doc above). 0.35
    // was chosen empirically as a reasonable starting point after the
    // first real-world report of raw unaccelerated deltas feeling far too
    // sensitive ("licin") versus the old warp-cursor path at the same
    // keymap speedRatio - expect this to need per-device/per-user tuning
    // via setSensitivityScale(), not to be exactly right out of the box.
    qreal m_sensitivityScale = 0.35;
};

#endif // WAYLANDMOUSETAP_H
