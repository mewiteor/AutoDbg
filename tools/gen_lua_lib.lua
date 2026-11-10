-- 功能：把 <lua dir> 下所有 .lua 编译成字节码，生成 datas.h / <out .c file>
--
-- 用法（用与目标运行时【同版本】的解释器运行）：
--   lua_build.exe gen_lua_lib.lua <out .c file> [lua files ...]
-- 例：
--   lua_build.exe gen_lua_lib.lua build/x64/release/gens/datas_lua.c lualib/jsonx.lua lualib/inspect.lua
--
-- ❗字节码格式取决于运行本脚本的解释器：
--   lua_flavor=lua    → 用 Lua5.5 解释器（你的自举 lua_build.exe）→ 5.5 字节码
--   lua_flavor=luajit → 必须用 luajit.exe 运行 → LuaJIT 字节码

-- ===================== 参数解析（对应 main） =====================
if #arg < 2 then
	io.stderr:write(
		string.format("用法: %s <out .c file> [lua files ...]\n", arg[0] or "gen_lua_lib.lua")
	)
	os.exit(1)
end

local outfile = arg[1]

if #outfile == 0 or outfile:sub(#outfile-1) ~= ".c" then
	io.stderr:write("<out file> 参数无效\n")
	os.exit(1)
end

local outdir = outfile:match("(.+[\\/])[^\\/]+%.c")

-- ===================== gen_h =====================
local function gen_h()
	local f = assert(io.open(outdir .. "datas.h", "w"))
	f:write("#ifndef _DATAS_H_\n")
	f:write("#define _DATAS_H_\n")
	f:write("\n")
	f:write("#include <stdlib.h>\n")
	f:write("\n")
	f:write("struct ByteCodeT\n")
	f:write("{\n")
	f:write("    const char *name;\n")
	f:write("    const char *path;\n")
	f:write("    const char *bytecode;\n")
	f:write("    size_t len;\n")
	f:write("};\n")
	f:write("\n")
	f:write("extern const struct ByteCodeT all_bytecodes[];\n")
	f:write("extern size_t all_bytecodes_count; \n")
	f:write("\n")
	f:write("#endif // _DATAS_H_")
	f:close()
end

-- ===================== gen_c =====================
local function gen_c()
	local f = assert(io.open(outfile, "w"))
	f:write('#include "datas.h"\n')
	f:write("\n")
	f:write("static const char datas[] =\n")
	f:write("{\n")

	local codes = {}
	local offset = 0

    for i=2,#arg do
        local filepath = arg[i]
        local filename = filepath:match(".+[\\/]([^\\/]+)")
		-- 对应 luaL_loadfile
		local func, err = loadfile(filepath)
		if not func then
			io.stderr:write(string.format('加载"%s"失败: %s\n', filepath, err or "?"))
		else
			-- 对应 string.dump(func, true)，第二个参数 1 = strip 调试信息
			local ok, bytecode = pcall(string.dump, func, true)
			if not ok then
				io.stderr:write(string.format('dump"%s"失败: %s\n', filepath, tostring(bytecode)))
			else
				-- 写字节码（每行 15 个，格式 0xXX,）
				f:write("    // " .. filename .. "\n")
				local sz = #bytecode
				for i = 1, sz do
					if (i - 1) % 15 == 0 then
						f:write("    ")
					end
					f:write(string.format("0x%02X,", bytecode:byte(i)))
					if (i - 1) % 15 == 14 or i == sz then
						f:write("\n")
					end
				end

				codes[#codes + 1] = {
					name = filename:match("(.+)%.[^.]*$") or filename,
					path = "@" .. filename,
					offset = offset,
					len = sz,
				}
				offset = offset + sz
			end
        end
    end

	f:write("};\n")
	f:write("\n")
	f:write("const struct ByteCodeT all_bytecodes[] =\n")
	f:write("{\n")
	for _, item in ipairs(codes) do
		f:write(string.format('    {"%s", "%s", datas + 0x%X, 0x%X},\n', item.name, item.path, item.offset, item.len))
	end
	f:write("};\n")
	f:write("\n")
	f:write("size_t all_bytecodes_count = " .. #codes .. ";\n")
	f:close()
end

-- ===================== 主流程 =====================
gen_h()
gen_c()
