// #include <QDesktopWidget>
#include <QApplication>
#include <QCursor>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QShortcut>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QWindow>
#include <QtWidgets/QHBoxLayout>
#include <span>

#if defined(Q_OS_WIN32)
#include <Windows.h>
#endif

#include "config.h"
#include "iconhelper.h"
#include "../render/qyuvopenglwidget.h"
#ifdef QSC_HAVE_WAYLAND_RELATIVE_POINTER
#include "../util/mousetap/waylandmousetap.h"
#endif
#include "toolform.h"
#include "mousetap/mousetap.h"
#include "ui_videoform.h"
#include "videoform.h"
#include "qtscrcpytelemetry.h"
#include "../QtScrcpyCore/src/device/device.h"
#include "../QtScrcpyCore/src/device/decoder/decoder.h"
#include "../QtScrcpyCore/src/device/decoder/videobuffer.h"


VideoForm::VideoForm(bool framelessWindow, bool skin, bool showToolbar, QWidget *parent) : QWidget(parent), ui(new Ui::videoForm), m_skin(skin)
{
    ui->setupUi(this);
    this->setStyleSheet("background-color: black;");
    initUI();
    installShortcut();
    updateShowSize(size());
    bool vertical = size().height() > size().width();
    this->show_toolbar = showToolbar;
    if (m_skin) {
        updateStyleSheet(vertical);
    }
    if (framelessWindow) {
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    }

    connect(qApp, &QGuiApplication::applicationStateChanged,
            this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });
        } else {
            cancelActiveInputs("application-inactive");
            setPlatformMouseGrab(false);
        }
    });
}

VideoForm::~VideoForm()
{
    cancelActiveInputs("destructor");
    m_cursorGrabRequested = false;
    setPlatformMouseGrab(false);
    delete ui;
}

void VideoForm::initUI()
{
    if (m_skin) {
        QPixmap phone;
        if (phone.load(":/res/phone.png")) {
            m_widthHeightRatio = 1.0f * phone.width() / phone.height();
        }

#ifndef Q_OS_OSX
        // mac下去掉标题栏影响showfullscreen
        // 去掉标题栏
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        // 根据图片构造异形窗口
        setAttribute(Qt::WA_TranslucentBackground);
#endif
    }

    m_videoWidget = new QYuvOpenGLWidget();
    
    // .data()
    if (m_videoWidget) {
        m_videoWidget.data()->setAttribute(Qt::WA_OpaquePaintEvent);
        m_videoWidget.data()->setAttribute(Qt::WA_NoSystemBackground);
        m_videoWidget.data()->hide();
    }

    // setWidget
    ui->keepRatioWidget->setWidget(m_videoWidget.data());
    ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

    // Parent QLabel
    m_fpsLabel = new QLabel(m_videoWidget.data());
    QFont ft;
    ft.setPointSize(15);
    ft.setWeight(QFont::Light);
    ft.setBold(true);
    m_fpsLabel->setFont(ft);
    m_fpsLabel->move(5, 15);
    m_fpsLabel->setMinimumWidth(100);
    m_fpsLabel->setStyleSheet(R"(QLabel {color: #00FF00; background: transparent;})");

    setMouseTracking(true);
    if (m_videoWidget) m_videoWidget.data()->setMouseTracking(true);
    ui->keepRatioWidget->setMouseTracking(true);
}

QRect VideoForm::getGrabCursorRect()
{
    QRect rc;
#if defined(Q_OS_WIN32)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(m_videoWidget.data()->pos()), m_videoWidget.data()->size());
    rc.setTopLeft(rc.topLeft() * m_videoWidget.data()->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * m_videoWidget.data()->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_OSX)
    rc = m_videoWidget.data()->geometry();
    rc.setTopLeft(ui->keepRatioWidget->mapToGlobal(rc.topLeft()));
    rc.setBottomRight(ui->keepRatioWidget->mapToGlobal(rc.bottomRight()));

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_LINUX)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(m_videoWidget.data()->pos()), m_videoWidget.data()->size());
    rc.setTopLeft(rc.topLeft() * m_videoWidget.data()->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * m_videoWidget.data()->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#endif
    return rc;
}

const QSize &VideoForm::frameSize()
{
    return m_frameSize;
}

void VideoForm::resizeSquare()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    resize(screenRect.height(), screenRect.height());
}

void VideoForm::removeBlackRect()
{
    resize(ui->keepRatioWidget->goodSize());
}

void VideoForm::showFPS(bool show)
{
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setVisible(show);
}

void VideoForm::updateRender(int width, int height, 
                             std::span<const uint8_t> dataY, 
                             std::span<const uint8_t> dataU, 
                             std::span<const uint8_t> dataV, 
                             int linesizeY, int linesizeU, int linesizeV)
{
    // 1. FAILSAFE
    if (m_videoWidget && m_videoWidget.data()->isHidden()) {
        QMetaObject::invokeMethod(this, [this](){
            if (m_videoWidget) {
                if (m_loadingWidget) m_loadingWidget->close();
                m_videoWidget.data()->show();
            }
        }, Qt::QueuedConnection);
    }

    // 2. RESIZE LOGIC
    if ((m_frameSize.width() != width || m_frameSize.height() != height) && !m_resizePending) {
        m_resizePending = true; // Kunci pintu antrian
        
        QMetaObject::invokeMethod(this, [this, width, height](){
            // Logika UI (Main Thread)
            updateShowSize(QSize(width, height));
            m_resizePending = false; // Buka kunci
        }, Qt::QueuedConnection);
    }

    // 3. DATA TRANSFER
    if (m_videoWidget) {
        m_videoWidget.data()->setFrameData(width, height, 
                                           dataY, dataU, dataV, 
                                           linesizeY, linesizeU, linesizeV);
    }
}

void VideoForm::setSerial(const QString &serial)
{
    m_serial = serial;

    auto deviceInterface = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!deviceInterface) {
        return;
    }

    qsc::Device* deviceImpl = static_cast<qsc::Device*>(deviceInterface.data());

    if (deviceImpl && deviceImpl->decoder() && m_videoWidget) {
        VideoBuffer* vb = deviceImpl->decoder()->videoBuffer();
        if (vb) {
        } else {
            qWarning() << "[SW] Failed: VideoBuffer is NULL!";
        }
    } else {
        qWarning() << "[SW] Failed: Could not access internal decoder.";
    }
}

void VideoForm::showToolForm(bool show)
{
    if (!m_toolForm) {
        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);
        m_toolForm->setSerial(m_serial);
        m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);
    }
    m_toolForm->setVisible(show);
    if (show) m_toolForm->raise();
}

void VideoForm::moveCenter()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    move(screenRect.center() - QRect(0, 0, size().width(), size().height()).center());
}

void VideoForm::installShortcut()
{
    QShortcut *shortcut = nullptr;

    // switchFullScreen
    shortcut = new QShortcut(QKeySequence("Ctrl+f"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        switchFullScreen();
    });

    // resizeSquare
    shortcut = new QShortcut(QKeySequence("Ctrl+g"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { resizeSquare(); });

    // removeBlackRect
    shortcut = new QShortcut(QKeySequence("Ctrl+w"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { removeBlackRect(); });

    // postGoHome
    shortcut = new QShortcut(QKeySequence("Ctrl+h"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoHome();
    });

    // postGoBack
    shortcut = new QShortcut(QKeySequence("Ctrl+b"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoBack();
    });

    // postAppSwitch
    shortcut = new QShortcut(QKeySequence("Ctrl+s"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postAppSwitch();
    });

    // postGoMenu
    shortcut = new QShortcut(QKeySequence("Ctrl+m"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoMenu();
    });

    // postVolumeUp
    shortcut = new QShortcut(QKeySequence("Ctrl+up"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeUp();
    });

    // postVolumeDown
    shortcut = new QShortcut(QKeySequence("Ctrl+down"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeDown();
    });

    // postPower
    shortcut = new QShortcut(QKeySequence("Ctrl+p"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postPower();
    });

    shortcut = new QShortcut(QKeySequence("Ctrl+o"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDisplayPower(false);
    });

    // expandNotificationPanel
    shortcut = new QShortcut(QKeySequence("Ctrl+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->expandNotificationPanel();
    });

    // collapsePanel
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->collapsePanel();
    });

    // copy
    shortcut = new QShortcut(QKeySequence("Ctrl+c"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCopy();
    });

    // cut
    shortcut = new QShortcut(QKeySequence("Ctrl+x"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCut();
    });

    // clipboardPaste
    shortcut = new QShortcut(QKeySequence("Ctrl+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDeviceClipboard();
    });

    // setDeviceClipboard
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->clipboardPaste();
    });
}

QRect VideoForm::getScreenRect()
{
    QRect screenRect;
    QScreen *screen = QGuiApplication::primaryScreen();
    QWidget *win = window();
    if (win) {
        QWindow *winHandle = win->windowHandle();
        if (winHandle) {
            screen = winHandle->screen();
        }
    }

    if (screen) {
        screenRect = screen->availableGeometry();
    }
    return screenRect;
}

void VideoForm::updateStyleSheet(bool vertical)
{
    if (vertical) {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-v.png) 150px 65px 85px 65px;
                     border-width: 150px 65px 85px 65px;
                 }
                 )");
    } else {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-h.png) 65px 85px 65px 150px;
                     border-width: 65px 85px 65px 150px;
                 }
                 )");
    }
    layout()->setContentsMargins(getMargins(vertical));
}

QMargins VideoForm::getMargins(bool vertical)
{
    QMargins margins;
    if (vertical) {
        margins = QMargins(10, 68, 12, 62);
    } else {
        margins = QMargins(68, 12, 62, 10);
    }
    return margins;
}

void VideoForm::updateShowSize(const QSize &newSize)
{
    if (m_frameSize != newSize) {
        m_frameSize = newSize;

        m_widthHeightRatio = 1.0f * newSize.width() / newSize.height();
        ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

        bool vertical = m_widthHeightRatio < 1.0f ? true : false;
        QSize showSize = newSize;
        QRect screenRect = getScreenRect();
        if (screenRect.isEmpty()) {
            qWarning() << "getScreenRect is empty";
            return;
        }
        if (vertical) {
            showSize.setHeight(qMin(newSize.height(), screenRect.height() - 200));
            showSize.setWidth(showSize.height() * m_widthHeightRatio);
        } else {
            showSize.setWidth(qMin(newSize.width(), screenRect.width() / 2));
            showSize.setHeight(showSize.width() / m_widthHeightRatio);
        }

        if (isFullScreen() && qsc::IDeviceManage::getInstance().getDevice(m_serial)) {
            switchFullScreen();
        }

        if (isMaximized()) {
            showNormal();
        }

        if (m_skin) {
            QMargins m = getMargins(vertical);
            showSize.setWidth(showSize.width() + m.left() + m.right());
            showSize.setHeight(showSize.height() + m.top() + m.bottom());
        }

        if (showSize != size()) {
            resize(showSize);
            if (m_skin) {
                updateStyleSheet(vertical);
            }
            moveCenter();
        }
    }
}

void VideoForm::switchFullScreen()
{
    if (isFullScreen()) {
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);
        }

        showNormal();
        resize(m_normalSize);
        move(m_fullScreenBeforePos);

        if (m_skin) {
            updateStyleSheet(m_frameSize.height() > m_frameSize.width());
        }
        showToolForm(this->show_toolbar);
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS);
#endif
    } else {
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
        }

        m_normalSize = size();
        m_fullScreenBeforePos = pos();

        showToolForm(false);
        if (m_skin) {
            layout()->setContentsMargins(0, 0, 0, 0);
        }
        showFullScreen();

#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
    }
}

bool VideoForm::isHost()
{
    if (!m_toolForm) {
        return false;
    }
    return m_toolForm->isHost();
}

void VideoForm::updateFPS(quint32 fps)
{
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setText(QString("FPS:%1").arg(fps));
}

void VideoForm::grabCursor(bool grab)
{
    m_cursorGrabRequested = grab;
    if (!grab) {
        setPlatformMouseGrab(false);
        return;
    }

    QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });
}

void VideoForm::cancelActiveInputs(const char *reason)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) device->cancelActiveInputs();

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][Input] ui-cancel"
                << "reason=" << reason
                << "serial=" << m_serial;
    }
}

#ifdef QSC_HAVE_WAYLAND_RELATIVE_POINTER
void VideoForm::ensureWaylandMouseTap()
{
    if (m_waylandMouseTap) return;

    QWindow *handle = windowHandle();
    if (!handle) {
        // Not shown yet / no native window backing this widget. Callers
        // (setPlatformMouseGrab) are expected to tolerate m_waylandMouseTap
        // staying null here and fall back to the warp-cursor path; this
        // will be retried the next time setPlatformMouseGrab(true) runs
        // (e.g. restorePlatformMouseGrab() re-invokes it whenever the
        // window regains focus/visibility).
        return;
    }

    m_waylandMouseTap = std::make_unique<WaylandMouseTap>(handle, this);
    connect(m_waylandMouseTap.get(), &WaylandMouseTap::rawMotion,
            this, &VideoForm::onWaylandRawMotion);

    // Handles a startup race specific to the very first grab attempt after
    // a fresh server connection: if the Wayland relative-pointer/
    // pointer-constraints globals haven't finished their registry bind yet
    // when grabCursor(true) first fires, enable(true) below returns false
    // and setPlatformMouseGrab() falls through to the warp-cursor path
    // (MouseTap::enableMouseEventTap + QCursor::setPos recenter). But
    // WaylandMouseTap keeps m_wantEnabled=true internally and
    // auto-retries the lock itself once the globals do become ready
    // (see onManagerActiveChanged() in waylandmousetap.cpp) - entirely on
    // its own schedule, without VideoForm's involvement. Without this
    // handler, that delayed native lock would activate *on top of* the
    // still-running warp-cursor fallback: the cursor stays warped to
    // center (from the fallback) while simultaneously receiving native
    // relative-motion deltas (from the newly-activated lock), which is
    // exactly the "stuck in the center but still tracking movement"
    // symptom. Tearing down the fallback here - only when it was the
    // fallback that was active, i.e. m_platformMouseGrabActive but NOT
    // m_waylandNativeLockActive - reconciles the two paths whenever this
    // delayed activation happens.
    connect(m_waylandMouseTap.get(), &WaylandMouseTap::lockStateChanged,
            this, [this](bool locked) {
        if (!locked) return;
        m_waylandNativeLockActive = true;
        if (m_platformMouseGrabActive) {
            MouseTap::getInstance()->enableMouseEventTap(getGrabCursorRect(), false);
        }
    });
}

void VideoForm::onWaylandRawMotion(QPointF delta)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device || !m_videoWidget) return;
    // Matches the frameSize()/showSize() source used by every other input
    // event call site below (mouseEvent/wheelEvent/keyEvent) for
    // consistency: m_videoWidget's own frameSize(), not VideoForm::frameSize().
    emit device->relativeMouseMoveEvent(delta, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());
}
#endif

void VideoForm::setPlatformMouseGrab(bool grab)
{
    if (!grab && !m_platformMouseGrabActive) return;

    const QString platform = QGuiApplication::platformName();
    const bool isWayland = platform.startsWith(QLatin1String("wayland"));

#ifdef QSC_HAVE_WAYLAND_RELATIVE_POINTER
    // Prefer the compositor-native relative-pointer lock on Wayland: it
    // reports raw, unaccelerated motion deltas directly and never moves the
    // visible cursor at all, avoiding both the periodic-recenter jitter and
    // the round-trip cost of QCursor::setPos() that the warp-based fallback
    // below relies on (Wayland does not let clients set the pointer
    // position at all in the general case - see setPlatformMouseGrab's
    // existing warp step further down, which is the reason this path
    // exists in the first place).
    if (isWayland) {
        if (grab) {
            ensureWaylandMouseTap();
            const bool locked = m_waylandMouseTap && m_waylandMouseTap->enable(true);
            m_waylandNativeLockActive = locked;
            m_platformMouseGrabActive = true;

            if (qsc::telemetry::enabled()) {
                qInfo() << "[Telemetry][Input] mouse-grab"
                        << "requested=" << grab
                        << "active=" << m_platformMouseGrabActive
                        << "strategy=" << (locked ? "wayland-native" : "recenter-fallback")
                        << "platform=" << platform;
            }

            if (locked) return; // native lock engaged, skip the warp-based path entirely
            // Native lock unavailable right now (protocols not bound yet,
            // or no surface/pointer resolvable) - fall through to the
            // existing warp-cursor path below so grab still does
            // *something* useful instead of silently no-op'ing. If the
            // protocols become ready later, the next grabCursor(true) call
            // (e.g. via restorePlatformMouseGrab()) will pick up the
            // native path automatically.
        } else {
            if (m_waylandNativeLockActive && m_waylandMouseTap) {
                m_waylandMouseTap->enable(false);
            }
            m_waylandNativeLockActive = false;
            m_platformMouseGrabActive = false;
            if (qsc::telemetry::enabled()) {
                qInfo() << "[Telemetry][Input] mouse-grab"
                        << "requested=" << grab
                        << "active=" << m_platformMouseGrabActive
                        << "strategy=" << "wayland-native"
                        << "platform=" << platform;
            }
            return; // nothing else to release - the warp path was never engaged
        }
    }
#endif

    // Keep the proven platform-native paths: ClipCursor on Windows and XCB
    // pointer grab on X11. Qt's QWindow mouse grab is intentionally not used:
    // the Wayland plugin rejects it for normal top-level windows and it also
    // duplicates the native Windows confinement path.
    MouseTap::getInstance()->enableMouseEventTap(getGrabCursorRect(), grab);
    m_platformMouseGrabActive = grab;

    // Native Wayland has no grab implementation in MouseTap. The existing
    // game-input path confines the pointer by periodically warping it back to
    // the video center, so seed that state immediately when custom mode starts.
    if (grab && m_videoWidget) {
        const QPoint center = m_videoWidget->mapToGlobal(
            m_videoWidget->rect().center());
        QCursor::setPos(center);
    }

    if (qsc::telemetry::enabled()) {
        qInfo() << "[Telemetry][Input] mouse-grab"
                << "requested=" << grab
                << "active=" << m_platformMouseGrabActive
                << "strategy=" << (isWayland ? "recenter" : "native")
                << "platform=" << platform;
    }
}

void VideoForm::restorePlatformMouseGrab()
{
    if (!m_cursorGrabRequested || !isVisible() || !isActiveWindow() ||
        QGuiApplication::applicationState() != Qt::ApplicationActive) {
        return;
    }
    if (!m_platformMouseGrabActive) setPlatformMouseGrab(true);
}

void VideoForm::onFrame(int width, int height, 
                        std::span<const uint8_t> dataY, 
                        std::span<const uint8_t> dataU, 
                        std::span<const uint8_t> dataV, 
                        int linesizeY, int linesizeU, int linesizeV)
{
    updateRender(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void VideoForm::staysOnTop(bool top)
{
    bool needShow = false;
    if (isVisible()) {
        needShow = true;
    }
    setWindowFlag(Qt::WindowStaysOnTopHint, top);
    if (m_toolForm) {
        m_toolForm->setWindowFlag(Qt::WindowStaysOnTopHint, top);
    }
    if (needShow) {
        show();
    }
}

bool VideoForm::event(QEvent *event)
{
    if (event) {
        switch (event->type()) {
        case QEvent::WindowDeactivate:
        case QEvent::Hide:
        case QEvent::Close:
            cancelActiveInputs("window-inactive");
            setPlatformMouseGrab(false);
            break;
        case QEvent::WindowActivate:
        case QEvent::Show:
            QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });
            break;
        case QEvent::WindowStateChange:
            if (windowState().testFlag(Qt::WindowMinimized)) {
                cancelActiveInputs("window-minimized");
                setPlatformMouseGrab(false);
            } else {
                QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });
            }
            break;
        default:
            break;
        }
    }
    return QWidget::event(event);
}

void VideoForm::mousePressEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (event->button() == Qt::MiddleButton) {
        if (device && !device->isCurrentCustomKeymap()) {
            device->postGoHome();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        if (device && !device->isCurrentCustomKeymap()) {
            device->postGoBack();
            return;
        }
    }

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif

    if (m_videoWidget && m_videoWidget.data()->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF mappedPos = m_videoWidget.data()->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());

        /*
        if (event->button() == Qt::LeftButton) {
            qreal x = localPos.x() / m_videoWidget.data()->size().width();
            qreal y = localPos.y() / m_videoWidget.data()->size().height();
            QString posTip = QString(R"("pos": {"x": %1, "y": %2})").arg(x).arg(y);
            qInfo() << posTip.toStdString().c_str();
        }
        */
    } else {
        if (event->button() == Qt::LeftButton) {
            m_dragPosition = globalPos.toPoint() - frameGeometry().topLeft();
            event->accept();
        }
    }
}

void VideoForm::mouseReleaseEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_dragPosition.isNull()) {
        if (!device) {
            return;
        }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
        if (!m_videoWidget) return;

        QPointF local = m_videoWidget.data()->mapFrom(this, localPos.toPoint());
        if (local.x() < 0) local.setX(0);
        if (local.x() > m_videoWidget.data()->width()) local.setX(m_videoWidget.data()->width());
        if (local.y() < 0) local.setY(0);
        if (local.y() > m_videoWidget.data()->height()) local.setY(m_videoWidget.data()->height());
        
        QMouseEvent newEvent(event->type(), local, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());
    } else {
        m_dragPosition = QPoint(0, 0);
    }
}

void VideoForm::mouseMoveEvent(QMouseEvent *event)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_videoWidget && m_videoWidget.data()->geometry().contains(event->pos())) {
        if (!device) return;
        
        QPointF mappedPos = m_videoWidget.data()->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());
    } else if (!m_dragPosition.isNull()) {
        if (event->buttons() & Qt::LeftButton) {
            move(globalPos.toPoint() - m_dragPosition);
            event->accept();
        }
    }
}

void VideoForm::mouseDoubleClickEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (event->button() == Qt::LeftButton && m_videoWidget && !m_videoWidget.data()->geometry().contains(event->pos())) {
        if (!isMaximized()) {
            removeBlackRect();
        }
    }

    if (event->button() == Qt::RightButton && device && !device->isCurrentCustomKeymap()) {
        emit device->postBackOrScreenOn(event->type() == QEvent::MouseButtonPress);
    }

    if (m_videoWidget && m_videoWidget.data()->geometry().contains(event->pos())) {
        if (!device) return;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif

        QPointF mappedPos = m_videoWidget.data()->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());
    }
}

void VideoForm::wheelEvent(QWheelEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!m_videoWidget) return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    if (m_videoWidget.data()->geometry().contains(event->position().toPoint())) {
        if (!device) return;
        QPointF pos = m_videoWidget.data()->mapFrom(this, event->position().toPoint());
        QWheelEvent wheelEvent(
            pos, event->globalPosition(), event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(), event->phase(), event->inverted());
#else
    if (m_videoWidget.data()->geometry().contains(event->pos())) {
        if (!device) return;
        QPointF pos = m_videoWidget.data()->mapFrom(this, event->pos());

        QWheelEvent wheelEvent(
            pos, event->globalPosF(), event->pixelDelta(), event->angleDelta(), event->delta(), event->orientation(),
            event->buttons(), event->modifiers(), event->phase(), event->source(), event->inverted());
#endif
        emit device->wheelEvent(&wheelEvent, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());
    }
}

void VideoForm::keyPressEvent(QKeyEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device || !m_videoWidget) return;
    
    if (Qt::Key_Escape == event->key() && !event->isAutoRepeat() && isFullScreen()) {
        switchFullScreen();
    }

    emit device->keyEvent(event, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());
}

void VideoForm::keyReleaseEvent(QKeyEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device || !m_videoWidget) return;

    emit device->keyEvent(event, m_videoWidget.data()->frameSize(), m_videoWidget.data()->size());
}

void VideoForm::paintEvent(QPaintEvent *paint)
{
    Q_UNUSED(paint)
    QStyleOption opt;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    opt.init(this);
#else
    opt.initFrom(this);
#endif
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void VideoForm::showEvent(QShowEvent *event)
{
    Q_UNUSED(event)
    QTimer::singleShot(0, this, [this]() { restorePlatformMouseGrab(); });
    if (!isFullScreen() && this->show_toolbar) {
        QTimer::singleShot(500, this, [this](){
            showToolForm(this->show_toolbar);
        });
    }
}

void VideoForm::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event)
    QSize goodSize = ui->keepRatioWidget->goodSize();
    if (goodSize.isEmpty()) {
        return;
    }
    QSize curSize = size();
    if (m_widthHeightRatio > 1.0f) {
        if (curSize.height() <= goodSize.height()) {
            setMinimumHeight(goodSize.height());
        } else {
            setMinimumHeight(0);
        }
    } else {
        if (curSize.width() <= goodSize.width()) {
            setMinimumWidth(goodSize.width());
        } else {
            setMinimumWidth(0);
        }
    }
}

void VideoForm::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    cancelActiveInputs("close");
    m_cursorGrabRequested = false;
    setPlatformMouseGrab(false);
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    Config::getInstance().setRect(device->getSerial(), geometry());
    device->disconnectDevice();
}

void VideoForm::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void VideoForm::dragMoveEvent(QDragMoveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dropEvent(QDropEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    const QMimeData *qm = event->mimeData();
    QList<QUrl> urls = qm->urls();

    for (const QUrl &url : urls) {
        QString file = url.toLocalFile();
        QFileInfo fileInfo(file);

        if (!fileInfo.exists()) {
            QMessageBox::warning(this, "QtScrcpy", tr("file does not exist"), QMessageBox::Ok);
            continue;
        }

        if (fileInfo.isFile() && fileInfo.suffix() == "apk") {
            emit device->installApkRequest(file);
            continue;
        }
        emit device->pushFileRequest(file, Config::getInstance().getPushFilePath() + fileInfo.fileName());
    }
}