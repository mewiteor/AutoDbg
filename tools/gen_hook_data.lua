assert(#arg == 2)
assert(arg[1]:sub(#arg[1]-1) == ".c")
assert(arg[2]:sub(#arg[2]-3) == ".dll")

local f = assert(io.open(arg[1]:sub(1,#arg[1]-1) .. "h", "w"))
f:write("#pragma once\n\nextern unsigned char g_dllMemory[];\n")
f:close()

f = assert(io.open(arg[1], "w"))
f:write("#include \"HookLibraryData.h\"\n\nunsigned char g_dllMemory[] =\n{\n")
local g = assert(io.open(arg[2], "rb"))
while true do
    local data = g:read(16)
    if data == nil then
        break
    end
    local s = "   "
    for i=1,#data do
        s = s .. string.format(" 0x%02X,", data:byte(i,i))
    end
    f:write(s .. "\n")
end
g:close()
f:write("};\n")
f:close()
