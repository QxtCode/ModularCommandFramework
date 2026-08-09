# ParmarPack — Parameter Passing

Every REGISTER_FUNC callback gets a `ParmarPack* pack`. Use it to read parameters and write results.

## Reading Parameters

```
Command:  -m:Calc -f:add -v:a|10,b|20
                              ↑    ↑
                            key  value
```

```cpp
REGISTER_FUNC("add", "a+b", {
    // One-liner: get with default
    int a = pack->GetAsOr<int>("a", 0);  // → 10
    int b = pack->GetAsOr<int>("b", 0);  // → 20

    // Check existence
    if (!pack->Has("a")) { /* error */ }

    // Get as optional
    auto v = pack->Get("key");       // std::optional<string>
    std::string s = pack->GetOr("key", "default");
});
```

## Writing Results

```cpp
// Success
pack->success = true;
pack->return_value = "done";

// Failure
pack->success = false;
pack->error.code    = ErrorCode::INVALID_PARAMS;  // 400
pack->error.message = "Missing parameter 'a'";
```

## Quick Reference

| Method | Returns | Example |
|--------|---------|---------|
| `Get("k")` | `optional<string>` | `auto v = pack->Get("name");` |
| `GetOr("k","d")` | `string` | `pack->GetOr("name","world")` |
| `GetAsOr<int>("k",0)` | `T` | `pack->GetAsOr<int>("a",0)` |
| `Has("k")` | `bool` | `if (pack->Has("flag"))` |
| `Set("k","v")` | void | `pack->Set("result","ok")` |
| `GetAll("k")` | `const vector<string>&` | multi-value key |

## Why not use old params directly?

```
// Old (4 lines):
auto it = pack->params.find("name");
string name = "world";
if (it != pack->params.end() && !it->second.empty())
    name = it->second[0];

// New (1 line):
string name = pack->GetOr("name", "world");
```

`params` is still public — old code won't break.
