#pragma once
// 八步 bring-up 测试序列入口（方案 §3.4）
// 返回 0=全部通过；非零=失败步骤号（1~8），任何异常应立即断电。
int bringup_run_all(void);

// 高优先级任务包装（main.c 用）：与 epdiy feed 线程同优先级跑完整序列，
// 避免渲染忙等轮转饿死低优先级主控。
void bringup_run_all_task(void *arg);
