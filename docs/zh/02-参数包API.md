# ParmarPack — 参数传递

每个 REGISTER_FUNC 回调都有一个 `ParmarPack* pack`，用它读参数、写结果。

## 读参数

```
命令:  -m:Calc -f:add -v:a|10,b|20
                             ↑    ↑
                           key  value
```

```cpp
REGISTER_FUNC("add", "a+b", {
    // 一行取值
    int a = pack->GetAsOr<int>("a", 0);  // → 10
    int b = pack->GetAsOr<int>("b", 0);  // → 20
    string s = pack->GetOr("key", "默认");
});
```

## 写结果

```cpp
pack->success = true;
pack->return_value = "处理完成";

// 失败
pack->success = false;
pack->error.code = ErrorCode::INVALID_PARAMS;
pack->error.message = "缺少参数";
```

## 速查

| 方法 | 示例 |
|------|------|
| `Get("k")` | `auto v = pack->Get("name");` |
| `GetOr("k","默认")` | `pack->GetOr("name","world")` |
| `GetAsOr<int>("k",0)` | `pack->GetAsOr<int>("a",0)` |
| `Has("k")` | `if (pack->Has("flag"))` |
| `Set("k","v")` | `pack->Set("result","ok")` |
