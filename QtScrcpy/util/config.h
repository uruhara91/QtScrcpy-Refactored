#ifndef CONFIG_H
#define CONFIG_H

#include <QObject>
#include <QRect>
#include <memory>

struct UserBootConfig
{
    QString recordPath = "";
    quint32 bitRate = 2000000;
    int maxSizeIndex = 0;
    int recordFormatIndex = 0;
    int lockOrientationIndex = 0;
    bool recordScreen     = false;
    bool recordBackground = false;
    bool reverseConnect   = true;
    bool showFPS          = false;
    bool windowOnTop      = false;
    bool autoOffScreen    = false;
    bool framelessWindow  = false;
    bool keepAlive        = false;
    bool useRoot          = false;
    bool simpleMode       = false;
    bool autoUpdateDevice = true;
    bool showToolbar      = true;
};

class QSettings;
class Config : public QObject
{
    Q_OBJECT
public:

    static Config &getInstance();

    // config
    QString getLanguage();
    QString getTitle();
    int getMaxFps();
    int getDesktopOpenGL();
    int getSkin();
    int getRenderExpiredFrames();
    QString getPushFilePath();
    QString getServerPath();
    QString getAdbPath();
    QString getLogLevel();
    QString getCodecOptions();
    QString getCodecName();
    QStringList getConnectedGroups();

    // user data:common
    void setUserBootConfig(const UserBootConfig &config);
    UserBootConfig getUserBootConfig();
    void setTrayMessageShown(bool shown);
    bool getTrayMessageShown();

    // user data:device
    void setNickName(const QString &serial, const QString &name);
    QString getNickName(const QString &serial);
    void setRect(const QString &serial, const QRect &rc);
    QRect getRect(const QString &serial);

    void deleteGroup(const QString &serial);

    // IP history methods
    void saveIpHistory(const QString &ip);
    QStringList getIpHistory(); 
    void clearIpHistory();

    // Port history methods
    void savePortHistory(const QString &port);
    QStringList getPortHistory(); 
    void clearPortHistory();

private:
    explicit Config(QObject *parent = nullptr);
    // Declared (not defaulted) so it can be defined in config.cpp, where
    // QSettings is a complete type. Needed because m_settings/m_userData
    // are std::unique_ptr<QSettings>: an implicitly-generated destructor
    // would need sizeof(QSettings) wherever it's instantiated, and thanks
    // to the Q_OBJECT meta-type machinery that happens in every
    // translation unit that includes this header (via moc-generated
    // code) - not just this one, which is the only one that included the
    // full <QSettings> header. QPointer (used here previously) didn't
    // have this requirement since it doesn't own/delete anything itself.
    ~Config() override;
    const QString &getConfigPath();

private:
    static QString s_configPath;
    // Constructed with `new QSettings(...)` and no Qt parent (see the
    // Config constructor) - a QPointer merely tracks an object, it does
    // not own/delete it, so these were never actually freed. unique_ptr
    // gives them a real owner: Config's implicitly-generated destructor.
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<QSettings> m_userData;
};

#endif // CONFIG_H
