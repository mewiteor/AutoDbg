-- =============================================================================
-- lualib/effects.lua —— 「客户端文件 I/O」结果解码（C 侧调，S7）
--
-- 来源：新写。
-- 背景：`read_file` / `write_file` 在 server 模式下会 `lua_yield` 一个效果表；
--   宿主（server）把它发给客户端执行，客户端回传 `POST /effects`，body 形如：
--     { "results": [ { "ok": true,  "data": "<base64>", "size": 1234 },
--                    { "ok": false, "error": "ENOENT: ..." } ] }
--   C 侧不写 JSON 解析器（D25/D28），所以把这段 JSON 交给本库解码，并**按 op**
--   转成「挂起的 Lua 函数应该返回的那几个值」：
--     read_file  → 成功：文件内容（二进制串）；失败：nil, errmsg
--     write_file → 成功：写入字节数；         失败：nil, errmsg
--   C 侧随后把这 N 个值 `lua_xmove` 到协程栈上再 `lua_resume`。
-- =============================================================================

local M = {}

-- ❗不能在文件顶部 `local json = jsonx`：lualib 是按**文件名字母序**烘焙的，
-- `effects.lua` 排在 `jsonx.lua` 之前 → 模块加载时 jsonx 还是 nil（会变成
-- “attempt to index a nil value (upvalue 'json')”）。改成调用时查全局。
local function json_decode(s)
  if type(jsonx) ~= "table" then
    error("jsonx is not loaded")
  end
  return jsonx.decode(s)
end

local ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local REV = {}
for i = 1, #ALPHABET do
  REV[ALPHABET:sub(i, i)] = i - 1
end

--- base64 解码（忽略空白与非法字符；容错截断）
local function b64decode(data)
  if type(data) ~= "string" then
    return nil
  end
  data = data:gsub("[^%w%+/=]", "")
  local out = {}
  for i = 1, #data, 4 do
    local c1, c2, c3, c4 = data:byte(i, i + 3)
    local v1 = c1 and REV[string.char(c1)]
    local v2 = c2 and REV[string.char(c2)]
    if not v1 or not v2 then
      break
    end
    local v3 = c3 and REV[string.char(c3)]
    local v4 = c4 and REV[string.char(c4)]
    out[#out + 1] = string.char(v1 * 4 + math.floor(v2 / 16))
    if v3 then
      out[#out + 1] = string.char((v2 % 16) * 16 + math.floor(v3 / 4))
    end
    if v4 then
      out[#out + 1] = string.char((v3 % 4) * 64 + v4)
    end
  end
  return table.concat(out)
end

M.b64decode = b64decode

--- 把客户端的结果 JSON 解成「挂起函数要返回的值」。
-- @param op   "read_file" | "write_file"
-- @param body 客户端回传的 JSON 文本
-- @return 变长返回值（见文件头）
function M.unpack(op, body)
  local ok, doc = pcall(json_decode, body or "")
  if not ok or type(doc) ~= "table" or type(doc.results) ~= "table" or #doc.results < 1 then
    return nil, "bad /effects body"
  end

  local r = doc.results[1]
  if type(r) ~= "table" then
    return nil, "bad /effects result"
  end

  if op == "write_file" then
    if r.ok then
      return tonumber(r.written) or -1
    end
    return nil, tostring(r.error or "client write failed")
  end

  -- 默认按 read_file 处理
  if r.ok then
    local decoded = b64decode(r.data)
    if decoded == nil then
      return nil, "bad base64 in read result"
    end
    return decoded
  end
  return nil, tostring(r.error or "client read failed")
end

return M
