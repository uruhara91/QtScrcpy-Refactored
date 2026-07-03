#include "waylandmousetap.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>

extern "C" {
#include <wayland-client-protocol.h>
}

namespace {
Q_LOGGING_CATEGORY(lcWaylandMouseTap, "qtscrcpy.waylandmousetap")

// Both zwp_relative_pointer_manager_v1 and zwp_pointer_constraints_v1 are at
// protocol version 1 as of the unstable-v1 specs bundled in protocols/.
constexpr int kProtocolVersion = 1;
} // namespace

// ---------------------------------------------------------------------
// WaylandRelativePointerManager
// ---------------------------------------------------------------------

WaylandRelativePointerManager::WaylandRelativePointerManager()
    : QWaylandClientExtensionTemplate<WaylandRelativePointerManager>(kProtocolVersion)
{
}

// ---------------------------------------------------------------------
// WaylandPointerConstraints
// ---------------------------------------------------------------------

WaylandPointerConstraints::WaylandPointerConstraints()
    : QWaylandClientExtensionTemplate<WaylandPointerConstraints>(kProtocolVersion)
{
}

// ---------------------------------------------------------------------
// WaylandRelativePointer
// ---------------------------------------------------------------------

WaylandRelativePointer::WaylandRelativePointer(struct ::zwp_relative_pointer_v1 *object, QObject *parent)
    : QObject(parent)
    , QtWayland::zwp_relative_pointer_v1(object)
{
}

WaylandRelativePointer::~WaylandRelativePointer()
{
    // Guard against double-destroy: this wrapper may be torn down after the
    // compositor connection itself is already gone (e.g. during
    // application shutdown), in which case object() can be null or the
    // request would be sent on a dead connection. destroy() only sends the
    // zwp_relative_pointer_v1.destroy request when we still hold a valid
    // object; QtWayland::zwp_relative_pointer_v1's own destructor does not
    // do this for us automatically.
    if (isInitialized()) destroy();
}

void WaylandRelativePointer::zwp_relative_pointer_v1_relative_motion(
    uint32_t utime_hi, uint32_t utime_lo,
    wl_fixed_t dx, wl_fixed_t dy,
    wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    Q_UNUSED(dx);
    Q_UNUSED(dy);
    // Prefer the unaccelerated delta (dx_unaccel/dy_unaccel): this is the
    // raw motion before the compositor/libinput's pointer-acceleration
    // curve is applied, which is what a locked-cursor FPS-style look
    // control wants - accelerated deltas would reintroduce exactly the
    // "floating"/non-linear feel this whole mechanism exists to avoid.
    const QPointF delta(wl_fixed_to_double(dx_unaccel), wl_fixed_to_double(dy_unaccel));
    const quint64 utimeUs = (static_cast<quint64>(utime_hi) << 32) | utime_lo;
    emit rawMotion(delta, utimeUs);
}

// ---------------------------------------------------------------------
// WaylandLockedPointer
// ---------------------------------------------------------------------

WaylandLockedPointer::WaylandLockedPointer(struct ::zwp_locked_pointer_v1 *object, QObject *parent)
    : QObject(parent)
    , QtWayland::zwp_locked_pointer_v1(object)
{
}

WaylandLockedPointer::~WaylandLockedPointer()
{
    if (isInitialized()) destroy();
}

void WaylandLockedPointer::zwp_locked_pointer_v1_locked()
{
    emit locked();
}

void WaylandLockedPointer::zwp_locked_pointer_v1_unlocked()
{
    emit unlocked();
}

// ---------------------------------------------------------------------
// WaylandMouseTap
// ---------------------------------------------------------------------

WaylandMouseTap::WaylandMouseTap(QWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
    // Defensive guard: QWaylandClientExtension's constructor is only safe
    // to call when a live Wayland platform integration exists underneath
    // QGuiApplication - constructing one without it (e.g. on X11, or an
    // "offscreen"/headless platform) segfaults inside Qt itself, not in
    // this class's own code (confirmed via an isolated crash-repro build:
    // the fault is in QWaylandClientExtension::QWaylandClientExtension,
    // reached through this constructor). VideoForm is expected to already
    // guard construction of WaylandMouseTap behind a
    // QGuiApplication::platformName() == "wayland" check, but this
    // constructor deliberately re-checks and refuses to even construct the
    // extension objects if that invariant is ever violated, rather than
    // relying solely on caller discipline for something this unsafe.
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        qCWarning(lcWaylandMouseTap, "WaylandMouseTap constructed on non-Wayland platform "
                                      "('%s') - refusing to bind Wayland protocol extensions; "
                                      "this instance will remain permanently non-functional "
                                      "(protocolsReady() will always return false)",
                  qUtf8Printable(QGuiApplication::platformName()));
        return;
    }

    m_relativePointerManager = std::make_unique<WaylandRelativePointerManager>();
    m_pointerConstraints = std::make_unique<WaylandPointerConstraints>();

    // Both extensions call initialize() internally on construction and will
    // asynchronously become active once the compositor's registry
    // advertisement round-trip completes - there is nothing further to
    // request here, just observe activeChanged() from both so
    // protocolsReady() can be recomputed as each one comes up (order
    // between the two is not guaranteed).
    connect(m_relativePointerManager.get(), &WaylandRelativePointerManager::activeChanged,
            this, &WaylandMouseTap::onManagerActiveChanged);
    connect(m_pointerConstraints.get(), &WaylandPointerConstraints::activeChanged,
            this, &WaylandMouseTap::onManagerActiveChanged);
}

WaylandMouseTap::~WaylandMouseTap()
{
    // Explicit, ordered teardown: the locked-pointer/relative-pointer
    // wrapper objects hold requests against the manager objects, so they
    // must be released before the managers themselves go away. Relying on
    // implicit unique_ptr destruction order (reverse-of-declaration, so
    // m_lockedPointer/m_relativePointer would actually already go first)
    // would be fragile if the member order above is ever reshuffled -
    // teardownLock() makes the ordering requirement explicit rather than
    // incidental.
    teardownLock();
}

bool WaylandMouseTap::protocolsReady() const
{
    return m_relativePointerManager && m_relativePointerManager->isActive() &&
           m_pointerConstraints && m_pointerConstraints->isActive();
}

WaylandMouseTap::NativeHandles WaylandMouseTap::resolveNativeHandles() const
{
    NativeHandles handles;

    if (!m_window) {
        qCWarning(lcWaylandMouseTap, "resolveNativeHandles: window has been destroyed");
        return handles;
    }

    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    if (!native) {
        qCWarning(lcWaylandMouseTap, "resolveNativeHandles: no platform native interface "
                                      "(not running under a QGuiApplication with a platform "
                                      "plugin loaded?)");
        return handles;
    }

    // wl_seat: the resource key is a fixed string ("wl_seat") documented by
    // Qt's Wayland platform plugin (qtwayland's QPlatformNativeInterface
    // implementation) - not part of a stable public API contract, but
    // materially unchanged since Qt 5 and confirmed against Qt 6 usage
    // (see e.g. blog.david-redondo.de's writeup on the successor
    // QNativeInterface::QWaylandApplication API, which only became
    // available starting in Qt 6.5 - this string-based path is used
    // instead specifically because it also works on the Qt 6.4 baseline
    // this project currently targets).
    auto *seat = static_cast<struct ::wl_seat *>(
        native->nativeResourceForIntegration(QByteArrayLiteral("wl_seat")));
    if (!seat) {
        qCDebug(lcWaylandMouseTap, "resolveNativeHandles: no wl_seat yet (compositor "
                                    "hasn't advertised one, or input focus not established)");
        return handles;
    }

    // wl_surface is deliberately re-resolved here rather than cached: Qt's
    // Wayland platform plugin destroys and recreates the underlying
    // wl_surface across window hide/show cycles, so a value fetched once
    // at construction time can silently go stale.
    auto *surface = static_cast<struct ::wl_surface *>(
        native->nativeResourceForWindow(QByteArrayLiteral("surface"), m_window));
    if (!surface) {
        qCDebug(lcWaylandMouseTap, "resolveNativeHandles: no wl_surface yet (window not "
                                    "shown/exposed yet?)");
        return handles;
    }

    // wl_seat_get_pointer() is a plain core-Wayland request (not part of
    // either unstable-v1 protocol), always available once we have a seat.
    // A fresh wl_pointer is requested each time rather than retained
    // long-term to keep lifetime handling simple and symmetric with the
    // surface handle above; wl_pointer objects are cheap and this is only
    // called when (re)establishing a lock, not per-frame.
    struct ::wl_pointer *pointer = wl_seat_get_pointer(seat);
    if (!pointer) {
        qCWarning(lcWaylandMouseTap, "resolveNativeHandles: wl_seat_get_pointer failed "
                                      "(seat has no pointer capability?)");
        return handles;
    }

    handles.surface = surface;
    handles.pointer = pointer;
    return handles;
}

bool WaylandMouseTap::enable(bool enabled)
{
    m_wantEnabled = enabled;

    if (!enabled) {
        teardownLock();
        return true;
    }

    if (isLocked()) return true; // already enabled, nothing to do

    if (!protocolsReady()) {
        qCDebug(lcWaylandMouseTap, "enable(true): protocols not ready yet, deferring "
                                    "(will not retry automatically - caller should call "
                                    "enable(true) again once protocolsReadyChanged(true) fires)");
        return false;
    }

    const NativeHandles handles = resolveNativeHandles();
    if (!handles.surface || !handles.pointer) {
        // wl_pointer ownership: on early return here, `handles.pointer` (if
        // non-null) would be leaked since nothing takes ownership of it
        // below. In practice this branch requires !handles.surface (the
        // only way to reach here with a non-null pointer is
        // surface==nullptr, since resolveNativeHandles returns early
        // whenever pointer acquisition fails), but release it
        // defensively regardless so this stays correct if that invariant
        // ever changes.
        if (handles.pointer) wl_pointer_release(handles.pointer);
        return false;
    }

    struct ::zwp_relative_pointer_v1 *relativePointerObject =
        m_relativePointerManager->get_relative_pointer(handles.pointer);
    if (!relativePointerObject) {
        qCWarning(lcWaylandMouseTap, "enable(true): get_relative_pointer request failed");
        wl_pointer_release(handles.pointer);
        return false;
    }
    m_relativePointer = std::make_unique<WaylandRelativePointer>(relativePointerObject);
    connect(m_relativePointer.get(), &WaylandRelativePointer::rawMotion,
            this, [this](QPointF delta, quint64 utimeUs) {
        Q_UNUSED(utimeUs);
        emit rawMotion(delta);
    });

    // lifetime_persistent: the lock survives the pointer leaving and
    // re-entering the surface region (which happens routinely with a
    // locked/invisible cursor moving relative to window bounds in ways the
    // compositor still tracks internally). lifetime_oneshot would drop the
    // lock the first time that happens, which is not the desired behavior
    // for a game-mode mouse-look that's expected to stay locked until the
    // user explicitly exits game mode.
    struct ::zwp_locked_pointer_v1 *lockedPointerObject =
        m_pointerConstraints->lock_pointer(
            handles.surface, handles.pointer, /*region=*/nullptr,
            QtWayland::zwp_pointer_constraints_v1::lifetime_persistent);
    if (!lockedPointerObject) {
        qCWarning(lcWaylandMouseTap, "enable(true): lock_pointer request failed");
        m_relativePointer.reset();
        wl_pointer_release(handles.pointer);
        return false;
    }
    m_lockedPointer = std::make_unique<WaylandLockedPointer>(lockedPointerObject);
    connect(m_lockedPointer.get(), &WaylandLockedPointer::locked,
            this, &WaylandMouseTap::onLockedPointerLocked);
    connect(m_lockedPointer.get(), &WaylandLockedPointer::unlocked,
            this, &WaylandMouseTap::onLockedPointerUnlocked);

    // The wl_pointer object itself is now only referenced by the two
    // requests above (get_relative_pointer, lock_pointer); per the Wayland
    // protocol both hold their own reference server-side once created, so
    // releasing our client-side handle here does not invalidate them. Not
    // releasing it would leak one wl_pointer object per enable() call.
    wl_pointer_release(handles.pointer);

    return true;
}

void WaylandMouseTap::teardownLock()
{
    // Order matters: release the relative-pointer subscription and the
    // lock together, but the lock (zwp_locked_pointer_v1) is the object
    // actually holding the compositor-side constraint - destroying it is
    // what visibly restores normal cursor behavior, so it is torn down
    // last to keep relative-motion events flowing for as long as the lock
    // itself is still nominally active.
    const bool wasLocked = isLocked();
    m_relativePointer.reset();
    m_lockedPointer.reset();
    if (wasLocked) emit lockStateChanged(false);
}

void WaylandMouseTap::onManagerActiveChanged()
{
    const bool ready = protocolsReady();
    if (ready == m_lastProtocolsReady) return;
    m_lastProtocolsReady = ready;
    emit protocolsReadyChanged(ready);

    // If the caller already asked for the lock to be enabled before the
    // protocols finished binding (a realistic race: game mode can be
    // toggled immediately on startup, before the compositor's registry
    // round-trip completes), retry now that we can actually attempt it.
    if (ready && m_wantEnabled && !isLocked()) enable(true);
}

void WaylandMouseTap::onLockedPointerLocked()
{
    emit lockStateChanged(true);
}

void WaylandMouseTap::onLockedPointerUnlocked()
{
    // The compositor can revoke a persistent lock on its own (e.g. the
    // surface losing focus, or policy decisions this client has no
    // visibility into) without enable(false) ever having been called. Tear
    // down our side to match reality rather than leaving a dangling
    // "locked" wrapper object whose requests would now be protocol
    // errors - but deliberately leave m_wantEnabled untouched so that a
    // future external trigger (e.g. the window regaining focus) can
    // re-enable it through the normal enable(true) path if the caller
    // chooses to retry.
    teardownLock();
}
