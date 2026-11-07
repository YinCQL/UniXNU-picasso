# GL surface 快速哈希实验（2026-11-07）

本次改动只为 `surface_changed()` 增加可选的快速哈希。使用四条独立的 64-bit
乘法累加链，最后混合结果，以缩短单条指令依赖链。`memcpy` 读取支持未对齐输入，
不足 8 字节的尾部也参与计算，读取不超出传入范围。

## 开关与边界

- 启动参数 `glh=1`：启用快速哈希。
- 无参数或 `glh=0`：继续使用原算法，默认关闭实验。
- IOSurface seed、检查频率、最多 8 帧的 backoff 和纹理上传规则保持原状。
- 两种算法都是非密码学变化检测器，存在哈希碰撞的可能。

提交范围为 EAGL 开关、哈希实现、主机测试及本文档。

## 验证

`scripts/tests/gl-surface-hash.c` 包含 8,224 个字节读取参考实现对照、
2,109,456 次逐字节修改检测及 514 个 guard-page 边界检查；普通优化构建及
AddressSanitizer / UndefinedBehaviorSanitizer 构建均通过。参考实现按小端读取，
测试用于当前 Apple Silicon / arm64 目标。

已有带计时诊断的真机 A/B 实验中，按字节归一化的哈希时间中位数从
0.344624 ns/B 降至 0.174195 ns/B，约减少 49.5%。活跃 heartbeat 仅为 4 / 5 个，
工作量并非逐像素相同，且 Data 分区 ENOSPC 干扰整体表现，因此结果只支持
哈希开销降低，不能据此声称稳定 60 FPS。本提交不包含这些计时诊断。

在 macOS 上运行主机测试：

```sh
xcrun clang -O2 -Wall -Wextra -Werror scripts/tests/gl-surface-hash.c \
  -o out/gl-exp01/gl-surface-hash-final-test
out/gl-exp01/gl-surface-hash-final-test

xcrun clang -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  scripts/tests/gl-surface-hash.c -o out/gl-exp01/gl-surface-hash-sanitized-test
out/gl-exp01/gl-surface-hash-sanitized-test
```

上述 `out/gl-exp01` 是已存在的外接归档目录符号链接。测试输出的微基准仅代表
主机性能；sanitizer 构建不用于比较性能，也不能将主机耗时外推到真机。
