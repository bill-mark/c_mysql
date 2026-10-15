# c_mysql —— cpro 的 MySQL 客户端

在 cpro 程序里连接 MySQL、执行 SQL、读取查询结果。

- 底层是 **MariaDB Connector/C**（`libmariadb`），本包是它的一层 cpro 封装，包名里的 `c_` 指的就是这个 C 客户端库
- 服务端只支持 **MySQL 8.0 及以后**
- **Linux 与 Windows** 都可用
- 函数名英文、拼音两套都导出，任选其一

```
import { connect, query } from "bill-mark/c_mysql"

rukou() {
    try {
        var c = connect({ user: "cpro", password: "cpro123", database: "cpro_ceshi" })
        var r = query(&c, "SELECT id, mingzi FROM yonghu WHERE nianling > ?", ["18"])
        for (row in r.rows) {
            print(unwrap(row[0], "NULL"), " | ", unwrap(row[1], "NULL"))
        }
    } catch (e) {
        print(e.msg)
    }
}
```

---

## 1. 装 C 客户端库

本包不自带 C 库，编译和运行的机器上都要先装好。

### Ubuntu / Debian

```bash
sudo apt install libmariadb-dev
```

验证：`ls /usr/include/mariadb/mysql.h` 能看到头文件，`ldconfig -p | grep libmariadb` 能看到库。

### Windows（MSYS2 UCRT64）

在 **UCRT64** 终端里：

```bash
pacman -S mingw-w64-ucrt-x86_64-libmariadbclient
```

必须是 `ucrt-x86_64` 版本。`mingw-w64-x86_64-*` 是 msvcrt 版，与 cpro 的运行时库混用会出问题。

编出来的 exe 运行时依赖 `libmariadb.dll`。拿到别的机器上运行时，把它和 exe 放在同一目录，或者确保它在 PATH 里。

---

## 2. 加入项目

```bash
cpro tianjia bill-mark/c_mysql@0.1.0
```

`cpro.json` 里会多出一行：

```json
"yilai": {
    "bill-mark/c_mysql": "0.1.0"
}
```

第三方包只能**具名导入**，用到哪个就导入哪个：

```
import { connect, exec, query, close } from "bill-mark/c_mysql"
import { lianjie, zhixing, chaxun, guanbi } from "bill-mark/c_mysql"   // 拼音，与上一行等价
```

与别的包重名时用 `as` 改名：`import { query as my_query } from "bill-mark/c_mysql"`。

---

## 3. 函数一览

| 英文 | 拼音 | 签名 | 说明 |
|---|---|---|---|
| `connect` | `lianjie` | `fn connect(opt Options) Conn fails` | 建立连接 |
| `exec` | `zhixing` | `fn exec(c &Conn, sql string, params [optional(string)] = []) ExecResult fails` | 执行不返回结果集的语句（INSERT / UPDATE / DELETE / DDL） |
| `query` | `chaxun` | `fn query(c &Conn, sql string, params [optional(string)] = []) Rows fails` | 执行查询，结果整个读进内存 |
| `column_index` | `lie_xuhao` | `fn column_index(r &Rows, name string) i64` | 按列名找列下标，找不到返回 -1 |
| `close` | `guanbi` | `fn close(c &Conn) bool fails` | 关闭连接（幂等） |

错误判定函数见第 7 节。

`close` 返回 `bool fails` 是因为 cpro 不支持 `void fails`。成功时恒为 `true`，可以忽略。

---

## 4. 连接

```
var c = connect({
    host:       "127.0.0.1",
    port:       3306,
    user:       "cpro",
    password:   "cpro123",
    database:   "cpro_ceshi",
    timeout_ms: 5000,
})
```

`Options` 的字段：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `host` | `string` | `"127.0.0.1"` | 主机名或 IP |
| `port` | `i64` | `3306` | 1..65535 |
| `user` | `string` | 无，必填 | |
| `password` | `string` | `""` | |
| `database` | `string` | `""` | 空串表示连上后不选库 |
| `timeout_ms` | `i64` | `10000` | 建立连接的超时。底层精度为秒，不足一秒按一秒算；`<= 0` 表示不限。只管连接阶段，执行慢查询不受它限制 |

字符集固定为 **utf8mb4**，在握手阶段就设好，不提供选项。中文和 emoji 可以直接读写。

认证方式支持 MySQL 8 默认的 `caching_sha2_password`，以及老的 `mysql_native_password`。

### 连接的生命周期

`Conn` 是 `nocopy` 类型，作用域结束、提前 `return`、`try` 传播错误时都会自动关闭。要提前关闭就调用 `close(&c)`，不要写 `destroy c`（nocopy 类型不能 destroy）。

`Conn` 也是**不透明类型**：只能通过 `var c = connect(...)` 推断持有，写不出它的类型名。所以目前：

- 写不出 `fn find_user(c &Conn, ...)` 这样的辅助函数，用到连接的代码要写在持有连接的那个函数里
- 连接不能放进数组，也就**没有连接池**
- 断线**不会自动重连**。收到"连接已断开"（第 7 节 `is_disconnected`）后需要重新 `connect`

---

## 5. 执行语句与参数

```
var r = exec(&c, "INSERT INTO yonghu (mingzi, nianling) VALUES (?, ?)", ["张三", "20"])
print(r.affected_rows)    // 1
print(r.insert_id)        // 新行的自增 id
```

`ExecResult` 的字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `affected_rows` | `i64` | 影响的行数 |
| `insert_id` | `i64` | 最后一次插入生成的自增 id；没有生成时为 0 |

### `?` 占位符

SQL 里的每个 `?` 按顺序换成 `params` 里的一个值：

- 有值：按当前连接的字符集转义，再加单引号，作为字符串字面量拼进 SQL
- `none`：拼成 `NULL`

```
var nianling = 20
exec(&c, "UPDATE yonghu SET nianling = ?, beizhu = ? WHERE id = ?",
     [number_to_string(nianling), none, "7"])
// 实际发出：UPDATE yonghu SET nianling = '20', beizhu = NULL WHERE id = '7'
```

**用户输入一律走 `?`，不要自己用 `+[string]` 拼进 SQL。** 转义只对 `?` 位置的值生效，自己拼接的字符串没有任何防护，会被 SQL 注入。

要点：

- 参数类型是 `[optional(string)]`。数字先用 `number_to_string` 转成文本，MySQL 会按列类型自动转换
- 写在引号（`'...'`、`"..."`、`` `...` ``）或注释里的 `?` 不算占位符
- `?` 的个数与 `params` 的长度不一致时报错（码 1），语句不会发出
- **`LIMIT` / `OFFSET` 不能用 `?`**。值会被加上引号，变成 `LIMIT '10'`，MySQL 不接受。分页数字请自己转成文本拼进 SQL；它是整数，不存在注入问题
- 表名、列名不能用 `?`，原因同上
- 一次只能执行一条语句，不支持用 `;` 分隔的多条语句

事务直接用 `exec` 执行对应语句：

```
exec(&c, "START TRANSACTION")
exec(&c, "UPDATE zhanghu SET yue = yue - 100 WHERE id = ?", ["1"])
exec(&c, "UPDATE zhanghu SET yue = yue + 100 WHERE id = ?", ["2"])
exec(&c, "COMMIT")
```

失败时在 `catch` 里执行 `ROLLBACK`。连接关闭时尚未提交的事务由服务端回滚。

---

## 6. 查询结果

```
var r = query(&c, "SELECT id, mingzi, beizhu FROM yonghu")
```

`Rows` 的字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `columns` | `[string]` | 列名，按 SELECT 的顺序 |
| `rows` | `[[optional(string)]]` | 每行一个数组，下标与 `columns` 对应；SQL 的 NULL 是 `none` |

```
var i_mingzi = column_index(&r, "mingzi")
for (row in r.rows) {
    print(unwrap(row[i_mingzi], "（空）"))
}
print(r.rows.length(), " 行")
```

**所有值都以文本返回**，就是 MySQL 客户端里看到的样子：

| 列类型 | 取到的值 |
|---|---|
| INT / BIGINT | `"42"`，要数字用 `string.parse_i64` |
| DECIMAL | `"12.50"`（保留精度，不会变成浮点数） |
| DATETIME | `"2026-10-14 08:00:00"` |
| BOOL / TINYINT(1) | `"1"` / `"0"` |
| NULL | `none` |

结果集**整个读进内存**再返回，适合常规的业务查询。几十万行的大结果请在 SQL 里分页。

cpro 的 `string` 必须是合法 UTF-8，所以**二进制列**（BLOB、VARBINARY 里存的非文本数据）读出来不是合法 UTF-8 时会报错（码 2）。第一版不支持读二进制数据。

`column_index` 按名字逐字比较、区分大小写。查询里有重名列（如两张表都有 `id`）时返回第一个，请用 `AS` 起别名。

---

## 7. 错误处理

可能失败的函数都是 `fails`，必须写在 `try` 里或用 `up_fails` 上抛。

`e.code` 的取值分两类：

**MySQL 自己的错误号**（1000 以上）原样作为 `e.code`，含义与 MySQL 官方文档一致，`e.msg` 带服务端的原文。常见的：

| 码 | 含义 |
|---|---|
| 1045 | 用户名或密码错误 |
| 1049 | 数据库不存在 |
| 1062 | 唯一键重复 |
| 1064 | SQL 语法错误 |
| 1146 | 表不存在 |
| 1205 | 等锁超时 |
| 1213 | 死锁 |
| 2002 / 2003 | 连不上服务端 |
| 2005 | 主机名解析失败 |
| 2006 / 2013 | 连接已断开 |

**本包自己的错误码**（小于 1000，不会与 MySQL 错误号冲突）：

| 码 | 含义 |
|---|---|
| 0 | 未分类 |
| 1 | `?` 的个数与参数个数不一致 |
| 2 | 结果中有非法 UTF-8（多为二进制列） |
| 3 | 连接已关闭（`close` 之后仍在使用） |
| 4 | 连接选项不合法（端口越界、`user` 为空） |

判定函数，参数是 `e.code`，返回 `bool`：

| 英文 | 拼音 | 为真的码 |
|---|---|---|
| `is_duplicate` | `shi_chongfu` | 1062 |
| `is_access_denied` | `shi_jujue_fangwen` | 1044、1045 |
| `is_connect_failed` | `shi_lianjie_shibai` | 2002、2003、2005 |
| `is_disconnected` | `shi_duankai` | 2006、2013 |
| `is_deadlock` | `shi_sisuo` | 1213 |
| `is_lock_timeout` | `shi_suo_chaoshi` | 1205 |

```
import { connect, exec, is_duplicate } from "bill-mark/c_mysql"

rukou() {
    try {
        var c = connect({ user: "cpro", password: "cpro123", database: "cpro_ceshi" })
        exec(&c, "INSERT INTO yonghu (youxiang) VALUES (?)", ["a@b.com"])
    } catch (e) {
        if (is_duplicate(e.code)) {
            print("邮箱已被注册")
        } else {
            print(e.msg)
        }
    }
}
```

---

## 8. 第一版不做的

- **TLS 加密连接**：连接是明文的，请只在本机或可信内网使用
- 服务端预处理语句（参数目前由客户端转义后拼接）
- 连接池、自动重连、异步与并发
- 读写二进制列
- 一次执行多条语句、存储过程返回的多个结果集

---

## 9. 许可

底层的 MariaDB Connector/C 是 LGPL-2.1 许可，本包以动态链接方式使用它。