#include "SecretStore.h"

#include <QtGlobal>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <wincred.h>
#  include <string>
#elif defined(Q_OS_MACOS)
#  include <CoreFoundation/CoreFoundation.h>
#  include <Security/Security.h>
#endif

namespace {
QString target(const QString &name) { return QStringLiteral("Relay/") + name; }
}

namespace SecretStore {

#if defined(Q_OS_WIN)

bool available() { return true; }

bool write(const QString &name, const QString &value)
{
    std::wstring t = target(name).toStdWString();
    const QByteArray blob = value.toUtf8();
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = &t[0];
    cred.CredentialBlobSize = DWORD(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(blob.constData()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    std::wstring user = L"Relay";
    cred.UserName = &user[0];
    return CredWriteW(&cred, 0) != FALSE;
}

QString read(const QString &name, bool *found)
{
    if (found) *found = false;
    const std::wstring t = target(name).toStdWString();
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(t.c_str(), CRED_TYPE_GENERIC, 0, &cred)) return QString();
    const QString value = QString::fromUtf8(reinterpret_cast<const char *>(cred->CredentialBlob),
                                            int(cred->CredentialBlobSize));
    CredFree(cred);
    if (found) *found = true;
    return value;
}

bool remove(const QString &name)
{
    const std::wstring t = target(name).toStdWString();
    return CredDeleteW(t.c_str(), CRED_TYPE_GENERIC, 0) != FALSE || GetLastError() == ERROR_NOT_FOUND;
}

#elif defined(Q_OS_MACOS)

namespace {
CFStringRef cfString(const QString &s)
{
    return s.toCFString();
}
CFMutableDictionaryRef query(const QString &name)
{
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = cfString(QStringLiteral("Relay"));
    CFStringRef account = cfString(name);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return q;
}
} // namespace

bool available() { return true; }

bool write(const QString &name, const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(bytes.constData()), bytes.size());
    CFMutableDictionaryRef q = query(name);
    CFMutableDictionaryRef update = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, data);
    OSStatus st = SecItemUpdate(q, update);
    if (st == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, data);
        st = SecItemAdd(q, nullptr);
    }
    CFRelease(update);
    CFRelease(q);
    CFRelease(data);
    return st == errSecSuccess;
}

QString read(const QString &name, bool *found)
{
    if (found) *found = false;
    CFMutableDictionaryRef q = query(name);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus st = SecItemCopyMatching(q, &result);
    CFRelease(q);
    if (st != errSecSuccess || !result) return QString();
    CFDataRef data = static_cast<CFDataRef>(result);
    const QString value = QString::fromUtf8(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                                            int(CFDataGetLength(data)));
    CFRelease(result);
    if (found) *found = true;
    return value;
}

bool remove(const QString &name)
{
    CFMutableDictionaryRef q = query(name);
    const OSStatus st = SecItemDelete(q);
    CFRelease(q);
    return st == errSecSuccess || st == errSecItemNotFound;
}

#else

bool available() { return false; }
bool write(const QString &, const QString &) { return false; }
QString read(const QString &, bool *found) { if (found) *found = false; return QString(); }
bool remove(const QString &) { return true; }

#endif

} // namespace SecretStore
