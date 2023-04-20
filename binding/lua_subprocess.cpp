#include <bee/error.h>
#include <bee/nonstd/filesystem.h>
#include <bee/subprocess.h>
#include <bee/subprocess/process_select.h>
#include <bee/utility/assume.h>
#include <binding/binding.h>
#include <binding/file.h>
#include <binding/udata.h>
#include <errno.h>
#include <signal.h>

#include <optional>
#if defined(_WIN32)
#    include <Windows.h>
#    include <bee/platform/win/unicode.h>
#    include <fcntl.h>
#    include <io.h>
#else
#    include <unistd.h>
#endif

namespace bee::lua {
    template <>
    struct udata<subprocess::process> {
        static inline int nupvalue = 1;
        static inline auto name    = "bee::subprocess";
    };
}

namespace bee::lua_subprocess {
    namespace process {
        static auto& to(lua_State* L, int idx) {
            return lua::checkudata<subprocess::process>(L, idx);
        }

        static void process_detach(lua_State* L, subprocess::process& process) {
            if (!process.detach()) {
                lua_pushfstring(L, "subprocess(%d) may become a zombie process", process.get_id());
                lua_warning(L, lua_tostring(L, -1), 0);
                lua_pop(L, 1);
            }
        }

        static int mt_close(lua_State* L) {
            auto& self = to(L, 1);
            process_detach(L, self);
            return 0;
        }

        static int mt_gc(lua_State* L) {
            auto& self = to(L, 1);
            process_detach(L, self);
            self.~process();
            return 0;
        }

        static int wait(lua_State* L) {
            auto& self  = to(L, 1);
            auto status = self.wait();
            if (status) {
                lua_pushinteger(L, (lua_Integer)*status);
                return 1;
            }
            auto error = make_syserror("subprocess::wait");
            lua_pushnil(L);
            lua_pushstring(L, error.c_str());
            return 2;
        }

        static int kill(lua_State* L) {
            auto& self  = to(L, 1);
            auto signum = lua::optinteger<int, SIGTERM>(L, 2);
            bool ok     = self.kill(signum);
            lua_pushboolean(L, ok);
            return 1;
        }

        static int get_id(lua_State* L) {
            auto& self = to(L, 1);
            lua_pushinteger(L, static_cast<lua_Integer>(self.get_id()));
            return 1;
        }

        static int is_running(lua_State* L) {
            auto& self = to(L, 1);
            lua_pushboolean(L, self.is_running());
            return 1;
        }

        static int resume(lua_State* L) {
            auto& self = to(L, 1);
            lua_pushboolean(L, self.resume());
            return 1;
        }

        static int native_handle(lua_State* L) {
            auto& self = to(L, 1);
#if defined(_WIN32)
            lua_pushlightuserdata(L, self.native_handle());
#else
            lua_pushlightuserdata(L, (void*)(intptr_t)self.native_handle());
#endif
            return 1;
        }

        static int mt_index(lua_State* L) {
            lua_pushvalue(L, 2);
            if (LUA_TNIL != lua_rawget(L, lua_upvalueindex(1))) {
                return 1;
            }
            if (LUA_TTABLE == lua_getiuservalue(L, 1, 1)) {
                lua_pushvalue(L, 2);
                if (LUA_TNIL != lua_rawget(L, -2)) {
                    return 1;
                }
            }
            return 0;
        }

        static int mt_newindex(lua_State* L) {
            if (LUA_TTABLE != lua_getiuservalue(L, 1, 1)) {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushvalue(L, -1);
                if (!lua_setiuservalue(L, 1, 1)) {
                    return 0;
                }
            }
            lua_insert(L, -3);
            lua_rawset(L, -3);
            return 0;
        }

        static void metatable(lua_State* L) {
            static luaL_Reg lib[] = {
                { "wait", wait },
                { "kill", kill },
                { "get_id", get_id },
                { "is_running", is_running },
                { "resume", resume },
                { "native_handle", native_handle },
                { NULL, NULL }
            };
            luaL_newlibtable(L, lib);
            luaL_setfuncs(L, lib, 0);
            lua_pushcclosure(L, mt_index, 1);
            lua_setfield(L, -2, "__index");
            static luaL_Reg mt[] = {
                { "__newindex", mt_newindex },
                { "__close", mt_close },
                { "__gc", mt_gc },
                { NULL, NULL }
            };
            luaL_setfuncs(L, mt, 0);
        }

        static int constructor(lua_State* L, subprocess::spawn& spawn) {
            lua::newudata<subprocess::process>(L, metatable, spawn);
            return 1;
        }
    }

    namespace spawn {
        static std::optional<lua::string_type> cast_cwd(lua_State* L) {
            lua_getfield(L, 1, "cwd");
            switch (lua_type(L, -1)) {
            case LUA_TSTRING: {
                auto ret = lua::checkstring(L, -1);
                lua_pop(L, 1);
                return ret;
            }
            case LUA_TUSERDATA: {
                auto const& path = lua::checkudata<fs::path>(L, -1);
                auto ret         = path.string<lua::string_type::value_type>();
                lua_pop(L, 1);
                return ret;
            }
            default:
                lua_pop(L, 1);
                return std::nullopt;
            }
        }

        static void cast_args_array(lua_State* L, int idx, subprocess::args_t& args) {
            const lua_Integer n = luaL_len(L, idx);
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_geti(L, idx, i);
                switch (lua_type(L, -1)) {
                case LUA_TSTRING:
                    args.push(lua::checkstrview(L, -1));
                    break;
                case LUA_TUSERDATA: {
                    auto const& path = lua::checkudata<fs::path>(L, -1);
                    args.push(path.string<lua::string_type::value_type>());
                    break;
                }
                case LUA_TTABLE:
                    cast_args_array(L, lua_absindex(L, -1), args);
                    break;
                default:
                    luaL_error(L, "Unsupported type: %s.", lua_typename(L, lua_type(L, -1)));
                    break;
                }
                lua_pop(L, 1);
            }
        }

        static subprocess::args_t cast_args(lua_State* L) {
            subprocess::args_t args;
            cast_args_array(L, 1, args);
            return args;
        }

        static luaL_Stream* get_file(lua_State* L, int idx) {
            void* p = lua_touserdata(L, idx);
            void* r = NULL;
            if (p) {
                if (lua_getmetatable(L, idx)) {
                    do {
                        luaL_getmetatable(L, "bee::file");
                        if (lua_rawequal(L, -1, -2)) {
                            r = p;
                            break;
                        }
                        lua_pop(L, 1);
                        luaL_getmetatable(L, LUA_FILEHANDLE);
                        if (lua_rawequal(L, -1, -2)) {
                            r = p;
                            break;
                        }
                    } while (false);
                    lua_pop(L, 2);
                }
            }
            luaL_argexpected(L, r != NULL, idx, LUA_FILEHANDLE);
            ASSUME(r != NULL);
            return (luaL_Stream*)r;
        }

        static file_handle cast_stdio(lua_State* L, const char* name, const file_handle handle) {
            switch (lua_getfield(L, 1, name)) {
            case LUA_TUSERDATA: {
                luaL_Stream* p = get_file(L, -1);
                if (!p->closef) {
                    lua_pop(L, 1);
                    return {};
                }
                return file_handle::dup(p->f);
            }
            case LUA_TBOOLEAN: {
                if (!lua_toboolean(L, -1)) {
                    break;
                }
                auto pipe = subprocess::pipe::open();
                if (!pipe) {
                    break;
                }
                lua_pop(L, 1);
                if (strcmp(name, "stdin") == 0) {
                    FILE* f = pipe.open_write();
                    if (!f) {
                        return {};
                    }
                    lua::newfile(L, f);
                    return pipe.rd;
                }
                else {
                    FILE* f = pipe.open_read();
                    if (!f) {
                        return {};
                    }
                    lua::newfile(L, f);
                    return pipe.wr;
                }
            }
            case LUA_TSTRING: {
                if (strcmp(name, "stderr") == 0 && strcmp(lua_tostring(L, -1), "stdout") == 0 && handle) {
                    lua_pop(L, 1);
                    lua_pushvalue(L, -1);
                    return handle;
                }
            }
            default:
                break;
            }
            lua_pop(L, 1);
            return {};
        }

        static file_handle cast_stdio(lua_State* L, subprocess::spawn& self, const char* name, subprocess::stdio type, const file_handle handle = {}) {
            file_handle f = cast_stdio(L, name, handle);
            if (f) {
                self.redirect(type, f);
            }
            return f;
        }

        static void cast_env(lua_State* L, subprocess::spawn& self) {
            subprocess::envbuilder builder;
            if (LUA_TTABLE == lua_getfield(L, 1, "env")) {
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    if (LUA_TSTRING == lua_type(L, -1)) {
                        builder.set(lua::checkstring(L, -2), lua::checkstring(L, -1));
                    }
                    else {
                        builder.del(lua::checkstring(L, -2));
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            self.env(builder.release());
        }

        static void cast_suspended(lua_State* L, subprocess::spawn& self) {
            if (LUA_TBOOLEAN == lua_getfield(L, 1, "suspended")) {
                if (lua_toboolean(L, -1)) {
                    self.suspended();
                }
            }
            lua_pop(L, 1);
        }

        static void cast_detached(lua_State* L, subprocess::spawn& self) {
            if (LUA_TBOOLEAN == lua_getfield(L, 1, "detached")) {
                if (lua_toboolean(L, -1)) {
                    self.detached();
                }
            }
            lua_pop(L, 1);
        }

#if defined(_WIN32)
        static void cast_option(lua_State* L, subprocess::spawn& self) {
            if (LUA_TSTRING == lua_getfield(L, 1, "console")) {
                auto console = lua::checkstrview(L, -1);
                if (console == "new") {
                    self.set_console(subprocess::console::eNew);
                }
                else if (console == "disable") {
                    self.set_console(subprocess::console::eDisable);
                }
                else if (console == "inherit") {
                    self.set_console(subprocess::console::eInherit);
                }
                else if (console == "detached") {
                    self.set_console(subprocess::console::eDetached);
                }
                else if (console == "hide") {
                    self.set_console(subprocess::console::eHide);
                }
            }
            lua_pop(L, 1);

            if (LUA_TBOOLEAN == lua_getfield(L, 1, "hideWindow")) {
                if (lua_toboolean(L, -1)) {
                    self.hide_window();
                }
            }
            lua_pop(L, 1);

            if (LUA_TBOOLEAN == lua_getfield(L, 1, "searchPath")) {
                if (lua_toboolean(L, -1)) {
                    self.search_path();
                }
            }
            lua_pop(L, 1);
        }
#else
        static void cast_option(lua_State*, subprocess::spawn&) {}
#endif

        static int spawn(lua_State* L) {
            luaL_checktype(L, 1, LUA_TTABLE);
            subprocess::spawn spawn;
            subprocess::args_t args = cast_args(L);
            if (args.size() == 0) {
                return 0;
            }

            auto cwd = cast_cwd(L);
            cast_env(L, spawn);
            cast_suspended(L, spawn);
            cast_option(L, spawn);
            cast_detached(L, spawn);

            file_handle f_stdin  = cast_stdio(L, spawn, "stdin", subprocess::stdio::eInput);
            file_handle f_stdout = cast_stdio(L, spawn, "stdout", subprocess::stdio::eOutput);
            file_handle f_stderr = cast_stdio(L, spawn, "stderr", subprocess::stdio::eError, f_stdout);
            if (!spawn.exec(args, cwd ? cwd->c_str() : 0)) {
                lua_pushnil(L);
                lua_pushstring(L, make_syserror("subprocess::spawn").c_str());
                return 2;
            }
            process::constructor(L, spawn);
            if (f_stderr) {
                lua_insert(L, -2);
                lua_setfield(L, -2, "stderr");
            }
            if (f_stdout) {
                lua_insert(L, -2);
                lua_setfield(L, -2, "stdout");
            }
            if (f_stdin) {
                lua_insert(L, -2);
                lua_setfield(L, -2, "stdin");
            }
            return 1;
        }
    }

    static int select(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_Integer n = luaL_len(L, 1);
        dynarray<subprocess::process*> set(static_cast<size_t>(n));
        for (int i = 0; i < static_cast<int>(n); ++i) {
            lua_geti(L, 1, i + 1);
            auto& p = lua::checkudata<subprocess::process>(L, -1);
            set[i] = &p;
            lua_pop(L, 1);
        }
        bool ok = subprocess::process_select(set);
        lua_pushboolean(L, ok);
        return 1;
    }

    static int peek(lua_State* L) {
        luaL_Stream* p = spawn::get_file(L, 1);
        if (!p->closef) {
            auto ec    = std::make_error_code(std::errc::broken_pipe);
            auto error = make_error(ec, "subprocess::peek");
            lua_pushnil(L);
            lua_pushstring(L, error.c_str());
            return 2;
        }
        int n = subprocess::pipe::peek(p->f);
        if (n < 0) {
            auto error = make_syserror("subprocess::peek");
            lua_pushnil(L);
            lua_pushstring(L, error.c_str());
            return 2;
        }
        lua_pushinteger(L, n);
        return 1;
    }

#if defined(_WIN32)
    static int filemode(lua_State* L) {
        luaL_Stream* p = spawn::get_file(L, 1);
        auto mode      = lua::checkstrview(L, 2);
        if (p && p->closef && p->f) {
            int ok = _setmode(_fileno(p->f), mode[0] == 'b' ? _O_BINARY : _O_TEXT);
            if (ok == -1) {
                lua_pushnil(L);
                lua_pushstring(L, make_crterror("_setmode").c_str());
                return 2;
            }
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushnil(L);
        lua_pushstring(L, make_error(std::make_error_code(std::errc::bad_file_descriptor), "_setmode").c_str());
        return 2;
    }
#else
    static int filemode(lua_State*) { return 0; }
#endif

    static int lsetenv(lua_State* L) {
        auto name  = lua::checkstrview(L, 1);
        auto value = lua::checkstrview(L, 2);
#if defined(_WIN32)
        lua_pushfstring(L, "%s=%s", name.data(), value.data());
        int ok = ::_putenv(lua_tostring(L, -1));
        if (ok == -1) {
            lua_pushnil(L);
            lua_pushstring(L, make_crterror("_putenv").c_str());
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
#else
        int ok = ::setenv(name.data(), value.data(), 1);
        if (ok == -1) {
            lua_pushnil(L);
            lua_pushstring(L, make_crterror("setenv").c_str());
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
#endif
    }

    static int get_id(lua_State* L) {
#if defined(_WIN32)
        lua_pushinteger(L, ::GetCurrentProcessId());
#else
        lua_pushinteger(L, ::getpid());
#endif
        return 1;
    }

    static const char script_quotearg[] = R"(
local s = ...
if type(s) ~= 'string' then
    s = tostring(s)
end
if #s == 0 then
    return '""'
end
if not s:find('[ \t\"]', 1) then
    return s
end
if not s:find('[\"\\]', 1) then
    return '"'..s..'"'
end
local quote_hit = true
local t = {}
t[#t+1] = '"'
for i = #s, 1, -1 do
    local c = s:sub(i,i)
    t[#t+1] = c
    if quote_hit and c == '\\' then
        t[#t+1] = '\\'
    elseif c == '"' then
        quote_hit = true
        t[#t+1] = '\\'
    else
        quote_hit = false
    end
end
t[#t+1] = '"'
for i = 1, #t // 2 do
    local tmp = t[i]
    t[i] = t[#t-i+1]
    t[#t-i+1] = tmp
end
return table.concat(t)
)";

    template <size_t N>
    static int lua_pushscript(lua_State* L, const char (&script)[N]) {
        if (luaL_loadbuffer(L, script, N - 1, "=module 'bee.subprocess'") != LUA_OK) {
            return lua_error(L);
        }
        return 1;
    }

    static int luaopen(lua_State* L) {
        static luaL_Reg lib[] = {
            { "spawn", spawn::spawn },
            { "select", select },
            { "peek", peek },
            { "filemode", filemode },
            { "setenv", lsetenv },
            { "get_id", get_id },
            { "quotearg", NULL },
            { NULL, NULL }
        };
        luaL_newlibtable(L, lib);
        luaL_setfuncs(L, lib, 0);

        lua_pushscript(L, script_quotearg);
        lua_setfield(L, -2, "quotearg");
        return 1;
    }
}

DEFINE_LUAOPEN(subprocess)
