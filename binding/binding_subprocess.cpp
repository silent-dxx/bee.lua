#include <bee/subprocess.h>
#include <bee/utility/unicode.h>
#include <lua.hpp>
#include <optional>
#include <errno.h>
#include <string.h>

#if defined(_WIN32)

typedef std::wstring nativestring;

std::wstring luaL_checknativestring(lua_State* L, int idx) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, idx, &len);
    return bee::u2w(std::string_view(str, len));
}

#else

typedef std::string nativestring;

std::string luaL_checknativestring(lua_State* L, int idx) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, idx, &len);
    return std::string(str, len);
}

#endif

namespace process {
    static int constructor(lua_State* L, bee::subprocess::spawn& spawn) {
        void* storage = lua_newuserdata(L, sizeof(bee::subprocess::process));
        luaL_getmetatable(L, "subprocess");
        lua_setmetatable(L, -2);
        new (storage)bee::subprocess::process(spawn);
        return 1;
    }

    static bee::subprocess::process& to(lua_State* L, int idx) {
        return *(bee::subprocess::process*)luaL_checkudata(L, idx, "subprocess");
    }

    static int destructor(lua_State* L) {
        bee::subprocess::process& self = to(L, 1);
        self.~process();
        return 0;
    }

    static int wait(lua_State* L) {
        bee::subprocess::process& self = to(L, 1);
        lua_pushinteger(L, (lua_Integer)self.wait());
        return 1;
    }

    static int kill(lua_State* L) {
        bee::subprocess::process& self = to(L, 1);
        bool ok = self.kill((int)luaL_optinteger(L, 2, 15));
        lua_pushboolean(L, ok);
        return 1;
    }

    static int get_id(lua_State* L) {
        bee::subprocess::process& self = to(L, 1);
        lua_pushinteger(L, (lua_Integer)self.get_id());
        return 1;
    }

    static int is_running(lua_State* L) {
        bee::subprocess::process& self = to(L, 1);
        lua_pushboolean(L, self.is_running());
        return 1;
    }

    static int resume(lua_State* L) {
        bee::subprocess::process& self = to(L, 1);
        lua_pushboolean(L, self.resume());
        return 1;
    }

    static int native_handle(lua_State* L) {
        bee::subprocess::process& self = to(L, 1);
        lua_pushinteger(L, self.native_handle());
        return 1;
    }
}

namespace spawn {
    static std::optional<nativestring> cast_cwd(lua_State* L) {
        if (LUA_TSTRING == lua_getfield(L, 1, "cwd")) {
            nativestring ret(luaL_checknativestring(L, -1));
            lua_pop(L, 1);
            return ret;
        }
        lua_pop(L, 1);
        return std::optional<nativestring>();
    }

    static int fileclose(lua_State* L) {
        luaL_Stream* p = (luaL_Stream*)luaL_checkudata(L, 1, LUA_FILEHANDLE);
        int ok = fclose(p->f);
        int en = errno;  /* calls to Lua API may change this value */
        if (ok) {
            lua_pushboolean(L, 1);
            return 1;
        }
        else {
            lua_pushnil(L);
            lua_pushfstring(L, "%s", strerror(en));
            lua_pushinteger(L, en);
            return 3;
        }
    }

    static int newfile(lua_State* L, FILE* f) {
        luaL_Stream* pf = (luaL_Stream*)lua_newuserdata(L, sizeof(luaL_Stream));
        luaL_setmetatable(L, LUA_FILEHANDLE);
        pf->closef = &fileclose;
        pf->f = f;
        return 1;
    }

#if defined(_WIN32)
    typedef std::vector<nativestring> native_args;
#   define LOAD_ARGS(L, idx) luaL_checknativestring((L), (idx))
#else
    typedef std::vector<char*> native_args;
#   define LOAD_ARGS(L, idx) (char*)luaL_checkstring((L), (idx))
#endif
    static void cast_args(lua_State* L, int idx, native_args& args) {
        lua_Integer n = luaL_len(L, idx);
        for (lua_Integer i = 1; i <= n; ++i) {
            switch (lua_geti(L, idx, i)) {
            case LUA_TSTRING:
                args.push_back(LOAD_ARGS(L, -1));
                break;
            case LUA_TTABLE:
                cast_args(L, lua_absindex(L, -1), args);
                break;
            }
            lua_pop(L, 1);
        }
    }

    static native_args cast_args(lua_State* L) {
        native_args args;
        cast_args(L, 1, args);
        return args;
    }

    static FILE* cast_stdio(lua_State* L, const char* name) {
        switch (lua_getfield(L, 1, name)) {
        case LUA_TUSERDATA: {
            luaL_Stream* p = (luaL_Stream*)luaL_checkudata(L, -1, LUA_FILEHANDLE);
            return p->f;
        }
        case LUA_TBOOLEAN: {
            if (!lua_toboolean(L, -1)) {
                break;
            }
            auto[rd, wr] = bee::subprocess::pipe::open();
            if (!rd || !wr) {
                break;
            }
            lua_pop(L, 1);
            if (strcmp(name, "stdin") == 0) {
                newfile(L, wr);
                return rd;
            }
            else {
                newfile(L, rd);
                return wr;
            }
        }
        default:
            break;
        }
        lua_pop(L, 1);
        return nullptr;
    }

    static void cast_env(lua_State* L, bee::subprocess::spawn& self) {
        if (LUA_TTABLE == lua_getfield(L, 1, "env")) {
            lua_next(L, 1);
            while (lua_next(L, -2)) {
                if (LUA_TSTRING == lua_type(L, -1)) {
                    self.env_set(luaL_checknativestring(L, -2), luaL_checknativestring(L, -1));
                }
                else {
                    self.env_del(luaL_checknativestring(L, -2));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    static void cast_suspended(lua_State* L, bee::subprocess::spawn& self) {
        if (LUA_TBOOLEAN == lua_getfield(L, 1, "suspended")) {
            if (lua_toboolean(L, -1)) {
                self.suspended();
            }
        }
        lua_pop(L, 1);
    }

#if defined(_WIN32)
    static void cast_option(lua_State* L, bee::subprocess::spawn& self)
    {
        if (LUA_TSTRING == lua_getfield(L, 1, "console")) {
            std::string console = luaL_checkstring(L, -1);
            if (console == "new") {
                self.set_console(bee::subprocess::console::eNew);
            }
            else if (console == "disable") {
                self.set_console(bee::subprocess::console::eDisable);
            }
            else if (console == "inherit") {
                self.set_console(bee::subprocess::console::eInherit);
            }
        }
        lua_pop(L, 1);

        if (LUA_TBOOLEAN == lua_getfield(L, 1, "windowHide")) {
            if (lua_toboolean(L, -1)) {
                self.hide_window();
            }
        }
        lua_pop(L, 1);
    }
#else
    static void cast_option(lua_State* , bee::subprocess::spawn&)
    { }
#endif

    static int spawn(lua_State* L) {
        luaL_checktype(L, 1, LUA_TTABLE);
        int retn = 0;
        bee::subprocess::spawn spawn;
        native_args args = cast_args(L);
        if (args.size() <= 1) {
            return 0;
        }

        std::optional<nativestring> cwd = cast_cwd(L);
        cast_env(L, spawn);
        cast_suspended(L, spawn);
        cast_option(L, spawn);

        FILE* f_stdin = cast_stdio(L, "stdin");
        if (f_stdin) {
            spawn.redirect(bee::subprocess::stdio::eInput, f_stdin);
            retn++;
        }
        FILE* f_stdout = cast_stdio(L, "stdout");
        if (f_stdout) {
            spawn.redirect(bee::subprocess::stdio::eOutput, f_stdout);
            retn++;
        }
        FILE* f_stderr = cast_stdio(L, "stderr");
        if (f_stderr) {
            spawn.redirect(bee::subprocess::stdio::eError, f_stderr);
            retn++;
        }
        if (!spawn.exec(args, cwd? cwd->c_str(): 0)) {
            return 0;
        }
        process::constructor(L, spawn);
        retn += 1;
        lua_insert(L, -retn);
        return retn;
    }
}

static int peek(lua_State* L) {
    luaL_Stream* p = (luaL_Stream*)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    lua_pushinteger(L, bee::subprocess::pipe::peek(p->f));
    return 1;
}

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>

static int filemode(lua_State* L) {
    luaL_Stream* p = (luaL_Stream*)luaL_checkudata(L, 1, LUA_FILEHANDLE);
    const char* mode = luaL_checkstring(L, 2);
    if (p && p->f) {
        if (mode[0] == 'b') {
            _setmode(_fileno(p->f), _O_BINARY);
        }
        else {
            _setmode(_fileno(p->f), _O_TEXT);
        }
    }
    return 0;
}
#else
static int filemode(lua_State* ) { return 0; }
#endif

extern "C"
#if defined(_WIN32)
__declspec(dllexport)
#endif
int luaopen_bee_subprocess(lua_State* L)
{
    static luaL_Reg mt[] = {
        { "wait", process::wait },
        { "kill", process::kill },
        { "get_id", process::get_id },
        { "is_running", process::is_running },
        { "resume", process::resume },
        { "native_handle", process::native_handle },
        { "__gc", process::destructor },
        { NULL, NULL }
    };
    luaL_newmetatable(L, "subprocess");
    luaL_setfuncs(L, mt, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    static luaL_Reg lib[] = {
        { "spawn", spawn::spawn },
        { "peek", peek },
        { "filemode", filemode },
        { NULL, NULL }
    };
    luaL_newlib(L, lib);
    return 1;
}