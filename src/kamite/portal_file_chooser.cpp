#include "kamite/portal_file_chooser.h"

#include <cstdio>
#include <system_error>

#if defined(__linux__) && !defined(__ANDROID__)
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#endif

namespace kamite::launcher::portal
{
    std::string requestPath(const std::string &uniqueName, const std::string &token)
    {
        std::string sender = uniqueName;
        if (!sender.empty() && sender.front() == ':')
            sender.erase(0, 1);
        for (char &c : sender)
            if (c == '.')
                c = '_';
        return "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    }

    bool pathFromUri(const std::string &uri, std::filesystem::path &out)
    {
        static const std::string scheme = "file://";
        if (uri.compare(0, scheme.size(), scheme) != 0)
            return false;
        size_t at = scheme.size();
        const size_t slash = uri.find('/', at);
        if (slash == std::string::npos)
            return false;
        const std::string host = uri.substr(at, slash - at);
        if (!host.empty() && host != "localhost")
            return false; // another machine's file: not one GHR can open
        std::string path;
        for (at = slash; at < uri.size(); ++at)
        {
            const char c = uri[at];
            if (c == '?' || c == '#')
                return false; // a query or fragment: no file URI the portal gives has one
            if (c != '%')
            {
                path += c;
                continue;
            }
            auto hex = [](char h) -> int
            {
                if (h >= '0' && h <= '9')
                    return h - '0';
                if (h >= 'a' && h <= 'f')
                    return h - 'a' + 10;
                if (h >= 'A' && h <= 'F')
                    return h - 'A' + 10;
                return -1;
            };
            if (at + 2 >= uri.size())
                return false; // "%" or "%4" at the end
            const int high = hex(uri[at + 1]), low = hex(uri[at + 2]);
            if (high < 0 || low < 0)
                return false;
            const char decoded = static_cast<char>(high * 16 + low);
            if (decoded == '\0')
                return false;
            path += decoded;
            at += 2;
        }
        if (path.empty() || path.front() != '/')
            return false;
        out = std::filesystem::path(path);
        return true;
    }

    std::string x11Parent(unsigned long xid)
    {
        if (xid == 0)
            return {};
        char text[32];
        std::snprintf(text, sizeof(text), "x11:%lx", xid);
        return text;
    }

    bool insideFlatpak()
    {
        std::error_code ec;
        return std::filesystem::exists("/.flatpak-info", ec);
    }

#if !defined(__linux__) || defined(__ANDROID__)
    Result openFile(const Request &)
    {
        Result result;
        result.error = "no desktop portal on this system";
        return result;
    }
#else
    namespace
    {
        // libdbus-1's C API, the calls used (dbus/dbus-*.h; the ABI has been stable since 1.0).
        struct DBusConnection;
        struct DBusMessage;
        struct DBusError // dbus-errors.h
        {
            const char *name;
            const char *message;
            unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
            void *padding1;
        };
        // dbus-message.h's DBusMessageIter is 72 bytes on x86-64 (2 pointers, a uint32, 12 ints, 2 pointers), filled in by
        // libdbus and only ever passed back to it: room to spare, aligned for its pointers.
        struct alignas(16) DBusMessageIter
        {
            unsigned char opaque[128];
        };
        using dbus_bool_t = uint32_t;
        constexpr int kBusSession = 0; // DBUS_BUS_SESSION
        constexpr int kTypeByte = 'y';
        constexpr int kTypeBoolean = 'b';
        constexpr int kTypeUint32 = 'u';
        constexpr int kTypeString = 's';
        constexpr int kTypeObjectPath = 'o';
        constexpr int kTypeArray = 'a';
        constexpr int kTypeVariant = 'v';
        constexpr int kTypeStruct = 'r';
        constexpr int kTypeDictEntry = 'e';

        constexpr const char *kPortal = "org.freedesktop.portal.Desktop";
        constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
        constexpr const char *kFileChooser = "org.freedesktop.portal.FileChooser";
        constexpr const char *kRequest = "org.freedesktop.portal.Request";

        struct LibDbus
        {
            void (*error_init)(DBusError *) = nullptr;
            void (*error_free)(DBusError *) = nullptr;
            dbus_bool_t (*error_is_set)(const DBusError *) = nullptr;
            DBusConnection *(*bus_get_private)(int, DBusError *) = nullptr;
            const char *(*bus_get_unique_name)(DBusConnection *) = nullptr;
            void (*bus_add_match)(DBusConnection *, const char *, DBusError *) = nullptr;
            void (*set_exit_on_disconnect)(DBusConnection *, dbus_bool_t) = nullptr;
            void (*connection_close)(DBusConnection *) = nullptr;
            void (*connection_unref)(DBusConnection *) = nullptr;
            dbus_bool_t (*read_write)(DBusConnection *, int) = nullptr;
            DBusMessage *(*pop_message)(DBusConnection *) = nullptr;
            DBusMessage *(*new_method_call)(const char *, const char *, const char *, const char *) = nullptr;
            DBusMessage *(*send_with_reply_and_block)(DBusConnection *, DBusMessage *, int, DBusError *) = nullptr;
            void (*message_unref)(DBusMessage *) = nullptr;
            dbus_bool_t (*is_signal)(DBusMessage *, const char *, const char *) = nullptr;
            const char *(*get_path)(DBusMessage *) = nullptr;
            const char *(*get_sender)(DBusMessage *) = nullptr;
            void (*iter_init_append)(DBusMessage *, DBusMessageIter *) = nullptr;
            dbus_bool_t (*iter_append_basic)(DBusMessageIter *, int, const void *) = nullptr;
            dbus_bool_t (*iter_open_container)(DBusMessageIter *, int, const char *, DBusMessageIter *) = nullptr;
            dbus_bool_t (*iter_close_container)(DBusMessageIter *, DBusMessageIter *) = nullptr;
            dbus_bool_t (*iter_init)(DBusMessage *, DBusMessageIter *) = nullptr;
            int (*iter_get_arg_type)(DBusMessageIter *) = nullptr;
            void (*iter_recurse)(DBusMessageIter *, DBusMessageIter *) = nullptr;
            dbus_bool_t (*iter_next)(DBusMessageIter *) = nullptr;
            void (*iter_get_basic)(DBusMessageIter *, void *) = nullptr;

            bool load(std::string &error)
            {
                void *lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
                if (!lib)
                {
                    const char *why = dlerror();
                    error = std::string("libdbus-1.so.3 did not load (") + (why ? why : "?") + ")";
                    return false;
                }
                bool all = true;
                auto get = [&](auto &fn, const char *symbol)
                {
                    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, symbol));
                    all = all && fn != nullptr;
                };
                get(error_init, "dbus_error_init");
                get(error_free, "dbus_error_free");
                get(error_is_set, "dbus_error_is_set");
                get(bus_get_private, "dbus_bus_get_private");
                get(bus_get_unique_name, "dbus_bus_get_unique_name");
                get(bus_add_match, "dbus_bus_add_match");
                get(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
                get(connection_close, "dbus_connection_close");
                get(connection_unref, "dbus_connection_unref");
                get(read_write, "dbus_connection_read_write");
                get(pop_message, "dbus_connection_pop_message");
                get(new_method_call, "dbus_message_new_method_call");
                get(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
                get(message_unref, "dbus_message_unref");
                get(is_signal, "dbus_message_is_signal");
                get(get_path, "dbus_message_get_path");
                get(get_sender, "dbus_message_get_sender");
                get(iter_init_append, "dbus_message_iter_init_append");
                get(iter_append_basic, "dbus_message_iter_append_basic");
                get(iter_open_container, "dbus_message_iter_open_container");
                get(iter_close_container, "dbus_message_iter_close_container");
                get(iter_init, "dbus_message_iter_init");
                get(iter_get_arg_type, "dbus_message_iter_get_arg_type");
                get(iter_recurse, "dbus_message_iter_recurse");
                get(iter_next, "dbus_message_iter_next");
                get(iter_get_basic, "dbus_message_iter_get_basic");
                if (!all)
                    error = "libdbus-1.so.3 lacks a function it needs";
                return all;
            }
        };

        std::string describe(const LibDbus &dbus, DBusError &e)
        {
            std::string out = dbus.error_is_set(&e) ? std::string(e.name ? e.name : "?") +
                                                          (e.message ? std::string(" (") + e.message + ")" : "")
                                                    : std::string("no reply");
            dbus.error_free(&e);
            return out;
        }

        // OpenFile's arguments: (s parent_window, s title, a{sv} options). Each step fails only when out of memory.
        class Writer
        {
        public:
            Writer(const LibDbus &dbus) : m_dbus(dbus) {}

            bool string(DBusMessageIter &iter, const std::string &value)
            {
                const char *text = value.c_str();
                return m_dbus.iter_append_basic(&iter, kTypeString, &text) != 0;
            }

            // One {sv} entry of the options, `fill` writing the variant's value of signature `type`.
            template <typename Fill> bool entry(DBusMessageIter &dict, const char *key, const char *type, Fill fill)
            {
                DBusMessageIter pair, variant;
                if (!m_dbus.iter_open_container(&dict, kTypeDictEntry, nullptr, &pair))
                    return false;
                bool ok = m_dbus.iter_append_basic(&pair, kTypeString, &key) != 0 &&
                          m_dbus.iter_open_container(&pair, kTypeVariant, type, &variant) != 0;
                if (ok)
                {
                    ok = fill(variant);
                    ok = m_dbus.iter_close_container(&pair, &variant) != 0 && ok;
                }
                return m_dbus.iter_close_container(&dict, &pair) != 0 && ok;
            }

            bool boolean(DBusMessageIter &iter, bool value)
            {
                const dbus_bool_t v = value ? 1u : 0u;
                return m_dbus.iter_append_basic(&iter, kTypeBoolean, &v) != 0;
            }

            // a(sa(us)): each filter's name and its (0 = glob, pattern) pairs.
            bool filters(DBusMessageIter &iter, const std::vector<Filter> &filters)
            {
                DBusMessageIter list;
                if (!m_dbus.iter_open_container(&iter, kTypeArray, "(sa(us))", &list))
                    return false;
                bool ok = true;
                for (const Filter &filter : filters)
                    ok = ok && one(list, filter);
                return m_dbus.iter_close_container(&iter, &list) != 0 && ok;
            }

            // ay, NUL-terminated (the portal reads it with g_variant_get_bytestring).
            bool bytes(DBusMessageIter &iter, const std::string &value)
            {
                DBusMessageIter array;
                if (!m_dbus.iter_open_container(&iter, kTypeArray, "y", &array))
                    return false;
                bool ok = true;
                for (size_t i = 0; i <= value.size() && ok; ++i)
                {
                    const unsigned char b = i < value.size() ? static_cast<unsigned char>(value[i]) : 0;
                    ok = m_dbus.iter_append_basic(&array, kTypeByte, &b) != 0;
                }
                return m_dbus.iter_close_container(&iter, &array) != 0 && ok;
            }

        private:
            bool one(DBusMessageIter &list, const Filter &filter)
            {
                DBusMessageIter item, globs;
                if (!m_dbus.iter_open_container(&list, kTypeStruct, nullptr, &item))
                    return false;
                bool ok = string(item, filter.name) && m_dbus.iter_open_container(&item, kTypeArray, "(us)", &globs);
                if (ok)
                {
                    for (const std::string &glob : filter.globs)
                    {
                        DBusMessageIter pair;
                        const uint32_t kind = 0; // a glob, not a MIME type
                        ok = ok && m_dbus.iter_open_container(&globs, kTypeStruct, nullptr, &pair);
                        if (!ok)
                            break;
                        ok = m_dbus.iter_append_basic(&pair, kTypeUint32, &kind) != 0 && string(pair, glob);
                        ok = m_dbus.iter_close_container(&globs, &pair) != 0 && ok;
                    }
                    ok = m_dbus.iter_close_container(&item, &globs) != 0 && ok;
                }
                return m_dbus.iter_close_container(&list, &item) != 0 && ok;
            }

            const LibDbus &m_dbus;
        };

        // The Response signal's (u response, a{sv} results): the first entry of results["uris"] (as).
        Result readResponse(const LibDbus &dbus, DBusMessage *signal)
        {
            Result result;
            DBusMessageIter args;
            if (!dbus.iter_init(signal, &args) || dbus.iter_get_arg_type(&args) != kTypeUint32)
            {
                result.error = "the portal's answer is not (u, a{sv})";
                return result;
            }
            uint32_t response = 2;
            dbus.iter_get_basic(&args, &response);
            if (response == 1)
            {
                result.outcome = Outcome::Cancelled;
                return result;
            }
            if (response != 0)
            {
                // "The user interaction was ended in some other way": the dialog is gone all the same, so no second
                // picker pops up after it.
                result.outcome = Outcome::Cancelled;
                result.error = "the dialog ended without a choice (response " + std::to_string(response) + ")";
                return result;
            }
            if (!dbus.iter_next(&args) || dbus.iter_get_arg_type(&args) != kTypeArray)
            {
                result.error = "the portal's answer has no results";
                return result;
            }
            DBusMessageIter dict;
            dbus.iter_recurse(&args, &dict);
            while (dbus.iter_get_arg_type(&dict) == kTypeDictEntry)
            {
                DBusMessageIter pair;
                dbus.iter_recurse(&dict, &pair);
                const char *key = nullptr;
                if (dbus.iter_get_arg_type(&pair) == kTypeString)
                    dbus.iter_get_basic(&pair, &key);
                if (key && std::string(key) == "uris" && dbus.iter_next(&pair) &&
                    dbus.iter_get_arg_type(&pair) == kTypeVariant)
                {
                    DBusMessageIter variant;
                    dbus.iter_recurse(&pair, &variant);
                    if (dbus.iter_get_arg_type(&variant) != kTypeArray)
                    {
                        result.error = "the portal's uris are not a list of strings";
                        return result;
                    }
                    DBusMessageIter uris;
                    dbus.iter_recurse(&variant, &uris);
                    if (dbus.iter_get_arg_type(&uris) != kTypeString)
                    {
                        result.outcome = Outcome::Cancelled;
                        result.error = "the portal answered with no file";
                        return result;
                    }
                    const char *uri = nullptr;
                    dbus.iter_get_basic(&uris, &uri);
                    std::filesystem::path path;
                    if (!uri || !pathFromUri(uri, path))
                    {
                        result.outcome = Outcome::Cancelled;
                        result.error = std::string("the chosen file is no local path: ") + (uri ? uri : "?");
                        return result;
                    }
                    result.outcome = Outcome::Chosen;
                    result.path = path;
                    return result;
                }
                dbus.iter_next(&dict);
            }
            result.outcome = Outcome::Cancelled;
            result.error = "the portal answered with no file";
            return result;
        }

        // A unique handle_token per request: the request path is built from it, so two dialogs never share one.
        std::string nextToken()
        {
            static std::atomic<unsigned> counter{0};
            return "ghr" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(++counter);
        }

        Result ask(const LibDbus &dbus, DBusConnection *connection, const Request &request)
        {
            Result result;
            const char *unique = dbus.bus_get_unique_name(connection);
            if (!unique || !*unique)
            {
                result.error = "the session bus gave no name";
                return result;
            }
            const std::string token = nextToken();
            std::string handle = requestPath(unique, token);

            // Listened for before the call: a portal may answer at once. The portal going away while the dialog is
            // up (NameOwnerChanged with no new owner) ends the wait.
            DBusError e;
            dbus.error_init(&e);
            dbus.bus_add_match(connection,
                               ("type='signal',interface='" + std::string(kRequest) + "',member='Response',path='" +
                                handle + "'")
                                   .c_str(),
                               &e);
            if (dbus.error_is_set(&e))
            {
                result.error = "listening for the answer failed: " + describe(dbus, e);
                return result;
            }
            dbus.bus_add_match(connection,
                               ("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                                "member='NameOwnerChanged',arg0='" + std::string(kPortal) + "'")
                                   .c_str(),
                               &e);
            if (dbus.error_is_set(&e))
            {
                result.error = "watching the portal failed: " + describe(dbus, e);
                return result;
            }

            DBusMessage *call = dbus.new_method_call(kPortal, kPortalPath, kFileChooser, "OpenFile");
            if (!call)
            {
                result.error = "out of memory";
                return result;
            }
            Writer w(dbus);
            DBusMessageIter args, options;
            dbus.iter_init_append(call, &args);
            bool built = w.string(args, request.parentWindow) && w.string(args, request.title) &&
                         dbus.iter_open_container(&args, kTypeArray, "{sv}", &options);
            if (built)
            {
                built = w.entry(options, "handle_token", "s", [&](DBusMessageIter &v) { return w.string(v, token); }) &&
                        w.entry(options, "modal", "b", [&](DBusMessageIter &v) { return w.boolean(v, true); });
                if (built && request.directory)
                    built = w.entry(options, "directory", "b", [&](DBusMessageIter &v) { return w.boolean(v, true); });
                if (built && !request.filters.empty())
                    built = w.entry(options, "filters", "a(sa(us))",
                                    [&](DBusMessageIter &v) { return w.filters(v, request.filters); });
                if (built && !request.currentFolder.empty())
                    built = w.entry(options, "current_folder", "ay",
                                    [&](DBusMessageIter &v) { return w.bytes(v, request.currentFolder); });
                built = dbus.iter_close_container(&args, &options) != 0 && built;
            }
            if (!built)
            {
                dbus.message_unref(call);
                result.error = "out of memory";
                return result;
            }
            DBusMessage *reply = dbus.send_with_reply_and_block(connection, call, request.replyTimeoutMs, &e);
            dbus.message_unref(call);
            if (!reply)
            {
                result.error = "OpenFile: " + describe(dbus, e);
                return result;
            }
            // Only the portal that answered may end the dialog: its unique name, from its own reply. A signal from
            // anyone else on the bus (sent straight to GHR, past the match rules) is ignored.
            const char *replier = dbus.get_sender(reply);
            const std::string portalName = replier ? replier : "";
            DBusMessageIter replyArgs;
            const char *path = nullptr;
            if (dbus.iter_init(reply, &replyArgs) && dbus.iter_get_arg_type(&replyArgs) == kTypeObjectPath)
                dbus.iter_get_basic(&replyArgs, &path);
            const std::string answered = path ? path : "";
            dbus.message_unref(reply);
            if (answered.empty() || portalName.empty())
            {
                result.error = "OpenFile's reply is not a request handle";
                return result;
            }
            if (answered != handle)
            {
                // A portal from before handle_token (xdg-desktop-portal < 0.9) picks its own path: listen there too.
                // Its answer only comes after the player has chosen, long after this.
                handle = answered;
                dbus.bus_add_match(connection,
                                   ("type='signal',interface='" + std::string(kRequest) + "',member='Response',path='" +
                                    handle + "'")
                                       .c_str(),
                                   &e);
                if (dbus.error_is_set(&e))
                {
                    result.error = "listening for the answer failed: " + describe(dbus, e);
                    return result;
                }
            }

            // The player's choice: no time limit, as zenity's and the Windows dialog's.
            for (;;)
            {
                if (!dbus.read_write(connection, 1000))
                {
                    result.error = "the session bus went away while the dialog was open";
                    return result;
                }
                while (DBusMessage *message = dbus.pop_message(connection))
                {
                    const char *sender = dbus.get_sender(message);
                    const char *at = dbus.get_path(message);
                    if (dbus.is_signal(message, kRequest, "Response") && sender && portalName == sender && at &&
                        handle == at)
                    {
                        result = readResponse(dbus, message);
                        dbus.message_unref(message);
                        return result;
                    }
                    if (dbus.is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged") && sender &&
                        std::string(sender) == "org.freedesktop.DBus")
                    {
                        DBusMessageIter change;
                        const char *name = nullptr, *before = nullptr, *after = nullptr;
                        if (dbus.iter_init(message, &change) && dbus.iter_get_arg_type(&change) == kTypeString)
                        {
                            dbus.iter_get_basic(&change, &name);
                            if (dbus.iter_next(&change) && dbus.iter_get_arg_type(&change) == kTypeString)
                                dbus.iter_get_basic(&change, &before);
                            if (dbus.iter_next(&change) && dbus.iter_get_arg_type(&change) == kTypeString)
                                dbus.iter_get_basic(&change, &after);
                        }
                        if (name && std::string(name) == kPortal && before && portalName == before)
                        {
                            dbus.message_unref(message);
                            result.error = "the portal went away while the dialog was open";
                            return result;
                        }
                    }
                    dbus.message_unref(message);
                }
            }
        }
    }

    Result openFile(const Request &request)
    {
        Result result;
        static LibDbus dbus;
        static std::string loadError;
        static const bool loaded = dbus.load(loadError);
        if (!loaded)
        {
            result.error = loadError;
            return result;
        }
        const char *address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        if (!address || !*address)
        {
            // libdbus's own default, $XDG_RUNTIME_DIR/bus, which it takes only when that is a socket the user owns
            // (_dbus_lookup_user_bus); anything else there and it would try X11 autolaunch (dbus-launch), starting a bus
            // of its own that no portal is on. So exactly libdbus's test, or no bus.
            const char *runtime = std::getenv("XDG_RUNTIME_DIR");
            struct stat st{};
            if (!runtime || !*runtime || ::stat((std::string(runtime) + "/bus").c_str(), &st) != 0 ||
                !S_ISSOCK(st.st_mode) || st.st_uid != ::getuid())
            {
                result.error = "no session bus ($DBUS_SESSION_BUS_ADDRESS unset, no $XDG_RUNTIME_DIR/bus socket of ours)";
                return result;
            }
        }
        DBusError e;
        dbus.error_init(&e);
        // A private connection, closed after the dialog: no other library's (SDL's) shared one is touched.
        DBusConnection *connection = dbus.bus_get_private(kBusSession, &e);
        if (!connection)
        {
            result.error = "no session bus (" + describe(dbus, e) + ")";
            return result;
        }
        dbus.set_exit_on_disconnect(connection, 0u);
        result = ask(dbus, connection, request);
        dbus.connection_close(connection);
        dbus.connection_unref(connection);
        return result;
    }
#endif
}
