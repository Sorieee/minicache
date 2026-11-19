/* main.c —— 程序入口
 *
 * M0 存根:只验证"能编译、能运行、能打日志"。
 * M1 将扩展为:命令行参数解析、initServer(监听 socket)、事件循环。
 */

#include "util.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    LOGI("mini-cache starting...");
    LOGD("nothing to serve yet, see docs/development-plan.md M1");
    return 0;
}
