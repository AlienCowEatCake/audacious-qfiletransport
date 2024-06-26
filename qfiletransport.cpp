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
        return QFileInfo(QUrl(str).toLocalFile()).absoluteFilePath();

    QByteArray tmp = str;
#if defined(_WIN32)
    /// @note For `file://C:\Users\User/.adplug/adplug.db`
    if (tmp.size() > 7 && tmp.startsWith("file://") && tmp[7] != '/')
        tmp = tmp.mid(7);
    /// @note For `/C:\some\path`
    if (tmp.size() > 2 && tmp[0] == '/' && tmp[2] == ':')
        tmp = tmp.mid(1);
#endif
    return QFileInfo(tmp).absoluteFilePath();
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
    if (!delim || !strcmp_nocase(uri, FILE_SCHEME, delim - uri))
    {
        /// @note Override scheme only for network files
        const QString localFile = toLocalFile(uri);
        if (localFile.startsWith("//") || localFile.startsWith("\\\\"))
        {
            /// @note Avoid earlier pluginEnabled() check due to possible
            /// deadlock if function is called from other plugins for some
            /// internal files or configs
            /// Example: `file://C:\Users\User/.adplug/adplug.db`
            /// https://github.com/AlienCowEatCake/audacious-qfiletransport/issues/1
            if (pluginEnabled())
            {
                StringBuf result;
                result.insert(0, HOOKED_SCHEME);
                return result;
            }
        }
    }
    return delim ? str_copy(uri, delim - uri) : StringBuf();
}

struct SearchParams
{
    QString filename;
    QStringList include, exclude;
};

static bool hasFrontCoverExtension(const QString & name)
{
    const QString ext = QFileInfo(name).suffix();
    if (ext.isEmpty())
        return false;

    static const QStringList exts = {"jpg", "jpeg", "png", "webp"};
    return exts.contains(ext, Qt::CaseInsensitive);
}

static bool coverNameFilter(const QString & name, const QStringList & keywords,
                            bool retOnEmpty)
{
    if (keywords.isEmpty())
        return retOnEmpty;

    for (const QString & keyword : keywords)
    {
        if (name.contains(keyword, Qt::CaseInsensitive))
            return true;
    }

    return false;
}

static bool sameBasename(const QString & a, const QString & b)
{
    const QString bnA = QFileInfo(a).completeBaseName();
    const QString bnB = QFileInfo(b).completeBaseName();
    return bnA.compare(bnB, Qt::CaseInsensitive) == 0;
}

static QString fileinfoRecursiveGetImage(const QString & path,
                                         const SearchParams * params, int depth)
{
    const QDir pathDir(path);
    if (!pathDir.exists() || !pathDir.isReadable())
        return QString();

    if (aud_get_bool("use_file_cover") && !depth)
    {
        // Look for images matching file name
        QDirIterator it(path, QDir::Files | QDir::Readable);
        while (!it.next().isEmpty())
        {
            const QFileInfo fileInfo = it.fileInfo();
            const QString name = fileInfo.fileName();
            if (hasFrontCoverExtension(name) &&
                sameBasename(name, params->filename))
            {
                return fileInfo.absoluteFilePath();
            }
        }
    }

    // Search for files using filter
    QDirIterator it(path, QDir::Files | QDir::Readable);
    while (!it.next().isEmpty())
    {
        const QFileInfo fileInfo = it.fileInfo();
        const QString name = fileInfo.fileName();
        if (hasFrontCoverExtension(name) &&
            coverNameFilter(name, params->include, true) &&
            !coverNameFilter(name, params->exclude, false))
        {
            return fileInfo.absoluteFilePath();
        }
    }

    if (aud_get_bool("recurse_for_cover") &&
        depth < aud_get_int("recurse_for_cover_depth"))
    {
        // Descend into directories recursively.
        QDirIterator it(path,
                        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
        while (!it.next().isEmpty())
        {
            const QFileInfo fileInfo = it.fileInfo();
            const QString newpath = fileInfo.absoluteFilePath();
            const QString tmp =
                fileinfoRecursiveGetImage(newpath, params, depth + 1);

            if (!tmp.isEmpty())
                return tmp;
        }
    }

    return QString();
}

static QStringList strListToQStringList(const char * list, const char * delims)
{
    const Index<String> index = str_list_to_index(list, delims);
    QStringList result;
    for (const String & str : index)
        result.append(QString::fromLocal8Bit(static_cast<const char *>(str)));
    return result;
}

static String art_search_patched(const char * filename)
{
    QString local = toLocalFile(filename);
    if (local.isEmpty())
        return String();

    const QString elem = QFileInfo(local).fileName();
    if (elem.isEmpty())
        return String();

    String include = aud_get_str("cover_name_include");
    String exclude = aud_get_str("cover_name_exclude");

    SearchParams params = {elem, strListToQStringList(include, ", "),
                           strListToQStringList(exclude, ", ")};

    local = QFileInfo(local).absolutePath();
    const QString imageLocal = fileinfoRecursiveGetImage(local, &params, 0);
    return imageLocal.isEmpty() ? String()
                                : String(fromLocalFile(imageLocal).constData());
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
        LPVOID originalAddressUriGetScheme = reinterpret_cast<LPVOID>(
            GetProcAddress(audcoreLib, "_Z14uri_get_schemePKc"));
        if (originalAddressUriGetScheme)
        {
            LPVOID patchedAddressUriGetScheme =
                reinterpret_cast<LPVOID>(&uri_get_scheme_patched);
            constexpr size_t jumpSize = 1 + sizeof(LPVOID);
            const size_t jumpOffset =
                reinterpret_cast<size_t>(patchedAddressUriGetScheme) -
                (reinterpret_cast<size_t>(originalAddressUriGetScheme) +
                 jumpSize);
            char patch[jumpSize];
            memcpy(patch, "\xE9", 1);
            memcpy(patch + 1, &jumpOffset, sizeof(LPVOID));
            WriteProcessMemory(GetCurrentProcess(), originalAddressUriGetScheme,
                               patch, jumpSize, Q_NULLPTR);
        }

        LPVOID originalAddressArtSearch = reinterpret_cast<LPVOID>(
            GetProcAddress(audcoreLib, "_Z10art_searchPKc"));
        if (originalAddressArtSearch)
        {
            LPVOID patchedAddressArtSearch =
                reinterpret_cast<LPVOID>(&art_search_patched);
            constexpr size_t jumpSize = 1 + sizeof(LPVOID);
            const size_t jumpOffset =
                reinterpret_cast<size_t>(patchedAddressArtSearch) -
                (reinterpret_cast<size_t>(originalAddressArtSearch) + jumpSize);
            char patch[jumpSize];
            memcpy(patch, "\xE9", 1);
            memcpy(patch + 1, &jumpOffset, sizeof(LPVOID));
            WriteProcessMemory(GetCurrentProcess(), originalAddressArtSearch,
                               patch, jumpSize, Q_NULLPTR);
        }
#elif defined(_M_X64)
        LPVOID originalAddressUriGetScheme = reinterpret_cast<LPVOID>(
            GetProcAddress(audcoreLib, "_Z14uri_get_schemePKc"));
        if (originalAddressUriGetScheme)
        {
            LPVOID patchedAddressUriGetScheme =
                reinterpret_cast<LPVOID>(&uri_get_scheme_patched);
            constexpr size_t jumpSize = 2 + sizeof(LPVOID) + 3;
            char patch[jumpSize];
            memcpy(patch, "\x49\xBA", 2);
            memcpy(patch + 2, &patchedAddressUriGetScheme, sizeof(LPVOID));
            memcpy(patch + 2 + sizeof(LPVOID), "\x41\xFF\xE2", 3);
            WriteProcessMemory(GetCurrentProcess(), originalAddressUriGetScheme,
                               patch, jumpSize, Q_NULLPTR);
        }

        LPVOID originalAddressArtSearch = reinterpret_cast<LPVOID>(
            GetProcAddress(audcoreLib, "_Z10art_searchPKc"));
        if (originalAddressArtSearch)
        {
            LPVOID patchedAddressArtSearch =
                reinterpret_cast<LPVOID>(&art_search_patched);
            constexpr size_t jumpSize = 2 + sizeof(LPVOID) + 3;
            char patch[jumpSize];
            memcpy(patch, "\x49\xBA", 2);
            memcpy(patch + 2, &patchedAddressArtSearch, sizeof(LPVOID));
            memcpy(patch + 2 + sizeof(LPVOID), "\x41\xFF\xE2", 3);
            WriteProcessMemory(GetCurrentProcess(), originalAddressArtSearch,
                               patch, jumpSize, Q_NULLPTR);
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
        "Copyright (C) 2023-2024 Peter S. Zhigalov",
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
