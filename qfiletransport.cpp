#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include <libaudcore/audstrings.h>
#include <libaudcore/i18n.h>
#include <libaudcore/interface.h>
#include <libaudcore/plugin.h>
#include <libaudcore/runtime.h>

static constexpr const char * const HOOKED_SCHEME = "qfiletransport";
static constexpr const char * const FILE_SCHEME = "file";

static QString toLocalFile(const QByteArray & str)
{
    const QUrl url(str);
    if (url.isValid() && url.isLocalFile())
        return QUrl(str).toLocalFile();
    return QFileInfo(str).absoluteFilePath();
}

static QByteArray fromLocalFile(const QString & str)
{
    return QUrl::fromLocalFile(str).toEncoded();
}

static bool pluginEnabled()
{
    for (PluginHandle * plugin : aud_plugin_list(PluginType::Transport))
    {
        if (!aud_plugin_get_enabled(plugin))
            continue;

        auto tp = reinterpret_cast<const TransportPlugin *>(
            aud_plugin_get_header(plugin));
        if (!tp)
            continue;

        for (const char * scheme : tp->schemes)
        {
            if (scheme && !strcmp(scheme, HOOKED_SCHEME))
                return true;
        }
    }
    return false;
}

static StringBuf uri_get_scheme_patched(const char * uri)
{
    const char * delim = strstr(uri, "://");
    if ((!delim || !strcmp_nocase(uri, FILE_SCHEME, delim - uri)) &&
        pluginEnabled())
    {
        StringBuf result;
        result.insert(0, HOOKED_SCHEME);
        return result;
    }
    return delim ? str_copy(uri, delim - uri) : StringBuf();
}

static void installUriSchemeHook()
{
#if defined(_WIN32)
#if !defined(AUDCORE_DLL_NAME)
#define AUDCORE_DLL_NAME "audcore"
#endif
    HINSTANCE audcoreLib = LoadLibraryA(AUDCORE_DLL_NAME ".dll");
    if (audcoreLib)
    {
#if defined(_M_IX86)
        LPVOID originalAddress = reinterpret_cast<LPVOID>(
            GetProcAddress(audcoreLib, "_Z14uri_get_schemePKc"));
        if (originalAddress)
        {
            LPVOID patchedAddress =
                reinterpret_cast<LPVOID>(&uri_get_scheme_patched);
            constexpr size_t jumpSize = 1 + sizeof(LPVOID);
            const size_t jumpOffset =
                reinterpret_cast<size_t>(patchedAddress) -
                (reinterpret_cast<size_t>(originalAddress) + jumpSize);
            char patch[jumpSize];
            memcpy(patch, "\xE9", 1);
            memcpy(patch + 1, &jumpOffset, sizeof(LPVOID));
            WriteProcessMemory(GetCurrentProcess(), originalAddress, patch,
                               jumpSize, Q_NULLPTR);
        }
#endif
        FreeLibrary(audcoreLib);
    }
#endif
}

class QFileTransport : public TransportPlugin
{
public:
#if !defined(PLUGIN_VERSION)
#define PLUGIN_VERSION ""
#endif
    static constexpr PluginInfo info = {
        "QFileTransport", "QFileTransport",
        "QFileTransport Plugin for Audacious " PLUGIN_VERSION "\n"
        "https://github.com/AlienCowEatCake/audacious-qfiletransport\n\n"
        "Copyright (C) 2023 Peter S. Zhigalov",
        Q_NULLPTR, PluginQtOnly};

    static constexpr const char * const schemes[] = {FILE_SCHEME,
                                                     HOOKED_SCHEME};

    QFileTransport() : TransportPlugin(info, schemes)
    {
        installUriSchemeHook();
    }

    void cleanup() Q_DECL_OVERRIDE
    {
        /// @note Clear timestamp from plugin registry to avoid plugin caching
        /// Cached plugins are lazy-loaded, but we need to install hook
        StringBuf path =
            filename_build({aud_get_path(AudPath::UserDir), "plugin-registry"});
        QFile file((QString(path)));
        if (!file.open(QFile::ReadWrite | QFile::Text))
            return;

        QString registryData = QString::fromUtf8(file.readAll());
        const QRegularExpression re("(qfiletransport\\.dll[\r\n]+stamp )[0-9]*",
                                    QRegularExpression::MultilineOption);
        const QRegularExpressionMatch match = re.match(registryData);
        if (match.hasMatch())
            registryData.replace(re, match.captured(1) + "0");
        file.seek(0);
        file.resize(0);
        file.write(registryData.toUtf8());
        file.close();
    }

    VFSImpl * fopen(const char * path, const char * mode,
                    String & error) Q_DECL_OVERRIDE
    {
        File * file = new File(path, mode);
        const QString errorString = file->errorString();
        if (!errorString.isEmpty())
        {
            delete file;
            file = Q_NULLPTR;
            error = String(errorString.toLocal8Bit().data());
        }
        return file;
    }

    VFSFileTest test_file(const char * filename, VFSFileTest test,
                          String & error) Q_DECL_OVERRIDE
    {
        int result = 0;
        const QFileInfo info(toLocalFile(filename));
        if (info.isFile())
            result |= VFS_IS_REGULAR;
        if (info.isSymLink())
            result |= VFS_IS_SYMLINK;
        if (info.isDir())
            result |= VFS_IS_DIR;
        if (info.isExecutable())
            result |= VFS_IS_EXECUTABLE;
        if (info.exists())
            result |= VFS_EXISTS;
        if (!info.isReadable())
            result |= VFS_NO_ACCESS;
        return VFSFileTest(test & result);
    }

    Index<String> read_folder(const char * filename,
                              String & error) Q_DECL_OVERRIDE
    {
        Index<String> result;
        const QString path = toLocalFile(filename);
        QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString file = it.next();
            if (it.fileInfo().isHidden())
                continue;
            result.append(fromLocalFile(file).constData());
        }
        return result;
    }

private:
    class File : public VFSImpl
    {
    public:
        File(const char * path, const char * mode) : m_file(toLocalFile(path))
        {
            QIODevice::OpenMode openMode = QIODevice::NotOpen;
            switch (mode[0])
            {
            case 'r':
                if (strchr(mode, '+'))
                    openMode |= QIODevice::ReadWrite | QIODevice::ExistingOnly;
                else
                    openMode |= QIODevice::ReadOnly | QIODevice::ExistingOnly;
                break;
            case 'w':
                if (strchr(mode, '+'))
                    openMode |= QIODevice::ReadWrite | QIODevice::Truncate;
                else
                    openMode |= QIODevice::WriteOnly | QIODevice::Truncate;
                break;
            case 'a':
                if (strchr(mode, '+'))
                    openMode |= QIODevice::ReadWrite | QIODevice::Append;
                else
                    openMode |= QIODevice::WriteOnly | QIODevice::Append;
                break;
            default:
                m_errorString = "Invalid open mode";
                return;
            }
            if (!m_file.open(openMode))
                m_errorString = "Error in QFile::open()";
        }

        QString errorString() const
        {
            return m_errorString;
        }

    protected:
        int64_t fread(void * ptr, int64_t size, int64_t nmemb) Q_DECL_OVERRIDE
        {
            qint64 total = 0;
            qint64 remain = size * nmemb;
            char * curr = reinterpret_cast<char *>(ptr);
            while (remain > 0)
            {
                qint64 part = m_file.read(curr, remain);
                if (part <= 0)
                    break;
                curr += part;
                total += part;
                remain -= part;
            }
            return (size > 0) ? total / size : 0;
        }

        int64_t fwrite(const void * buf, int64_t size,
                       int64_t nitems) Q_DECL_OVERRIDE
        {
            qint64 total = 0;
            qint64 remain = size * nitems;
            const char * curr = reinterpret_cast<const char *>(buf);
            while (remain > 0)
            {
                qint64 part = m_file.write(curr, remain);
                if (part <= 0)
                    break;
                curr += part;
                total += part;
                remain -= part;
            }
            return (size > 0) ? total / size : 0;
        }

        int fseek(int64_t offset, VFSSeekType whence) Q_DECL_OVERRIDE
        {
            qint64 pos = 0;
            switch (whence)
            {
            case VFS_SEEK_SET:
                pos = offset;
                break;
            case VFS_SEEK_CUR:
                pos = m_file.pos() + offset;
                break;
            case VFS_SEEK_END:
                pos = m_file.size() + offset;
                break;
            default:
                return -1;
            }
            return m_file.seek(pos) ? 0 : -1;
        }

        int64_t ftell() Q_DECL_OVERRIDE
        {
            return m_file.pos();
        }

        bool feof() Q_DECL_OVERRIDE
        {
            return m_file.atEnd();
        }

        int ftruncate(int64_t length) Q_DECL_OVERRIDE
        {
            return m_file.resize(length) ? 0 : -1;
        }

        int64_t fsize() Q_DECL_OVERRIDE
        {
            return m_file.size();
        }

        int fflush() Q_DECL_OVERRIDE
        {
            return m_file.flush() ? 0 : -1;
        }

    private:
        QFile m_file;
        QString m_errorString;
    };
};

EXPORT QFileTransport aud_plugin_instance;
