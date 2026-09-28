# sim_joystick

独立的 Linux C++ 键盘虚拟手柄，供 `unitree_mujoco` 的 Xbox 手柄入口使用。
通过 `/dev/uinput` 创建真正的输入设备，MuJoCo 从 `/dev/input/jsN` 读取它，
再将手柄信息写进 `rt/lowstate.wireless_remote`。无需修改 MuJoCo 或 `go2_ctrl`。
不依赖 ROS、DDS、SDK 或额外的 C++ 库。

## 编译

在 `unitree_rl_lab` 根目录执行：

```bash
cmake -S deploy/sim_joystick -B deploy/sim_joystick/build
cmake --build deploy/sim_joystick/build -j
```

## 启动

先加载虚拟输入和 joystick 驱动，再在终端 1 启动程序：

```bash
sudo modprobe uinput
sudo modprobe joydev
sudo ./deploy/sim_joystick/build/sim_joystick
```

`/dev/uinput` 通常只有 root 可写，以上 sudo 仅用于虚拟手柄工具。
若系统已经给当前用户授予了该设备权限，可以直接运行。
程序会显示 **本次生成的设备路径**，例如：

```text
Virtual gamepad: /dev/input/js0
```

在 `unitree_mujoco/simulate/config.yaml` 中配置：

```yaml
robot: "go2"
domain_id: 0
interface: "lo"
use_joystick: 1
joystick_type: "xbox"
joystick_device: "/dev/input/js0" # 使用程序显示的实际路径
joystick_bits: 16
```

如果运行 MuJoCo 的用户不能读取这个设备，可以临时仅为该用户授予读取权限：

```bash
# 将 js0 替换成实际设备名；在普通用户终端执行。
sudo setfacl -m "u:$(id -un):r" /dev/input/js0
```

若没有 `setfacl`，可以安装 Ubuntu 的 `acl` 软件包。设备重建后需要重新授权。
然后在终端 2 启动仿真，在终端 3 启动控制器：

```bash
# 终端 2
cd /home/witsir/DEV/PyProjects/work/unitree_mujoco/simulate/build
./unitree_mujoco -i 0 -n lo -r go2 -s scene.xml
```

```bash
# 终端 3
cd /home/witsir/DEV/PyProjects/work/unitree_rl_lab/deploy/robots/go2/build
./go2_ctrl --network lo
```

先启动虚拟手柄，再启动 MuJoCo。等待控制器初始化完成，回到 **终端 1** 操作。
不要把按键输入到 MuJoCo 窗口或控制器终端。
若重启虚拟手柄程序，设备会重新创建，需核对路径并重启 MuJoCo。

## 按键

| 键 | 手柄输入 | Go2 默认配置的作用 |
| --- | --- | --- |
| `1` | LT + A，持续 200 ms 后释放 | Passive → FixStand |
| `2` | Start，持续 200 ms 后释放 | FixStand → Velocity |
| `0` | LT + B，持续 200 ms 后释放 | FixStand/Velocity → Passive（阻尼，可能趴下） |
| `W / S` | 左摇杆纵向 | 前进 / 后退 |
| `A / D` | 左摇杆横向 | 左移 / 右移 |
| `Q / E` | 右摇杆横向 | 左转 / 右转 |
| 空格 | 轴归零，释放按钮 | 停止移动，保持当前控制模式 |
| `X` 或 `Ctrl+C` | 清空输入，销毁设备 | 退出虚拟手柄程序 |

按 `1` 后等待站立完成，再按 `2`。这些行为由控制器现有配置决定；若禁用了
Velocity，`2` 不会启动策略。模型缺失/不兼容仍需单独解决。

终端没有按键释放事件，因此移动键采用 **每个轴独立的 400 ms 超时**：点按产生
短暂移动，持续输入/键盘自动重复维持移动，停止输入后归零。首次长按时，若系统
键盘重复延迟超过 400 ms，移动可能短暂停顿。组合键取决于终端收到的重复字符，
不等同于实体手柄的多键持续按下。按钮命令同时清空速度；空格不会切换到阻尼。
退出只会清空手柄输入，不会自动给控制器发送阻尼命令；需要阻尼时先按 `0`。

默认摇杆幅度为 0.5，使用以下参数调整（0～1）：

```bash
sudo ./deploy/sim_joystick/build/sim_joystick --axis-value 0.3
```

这是摇杆值，不是保证达到的实际速度；策略部署配置可能进一步限制速度。

## 映射与检查

匹配当前 `unitree_mujoco/simulate/src/physics_joystick.h` 的 `XBoxJoystick`：

- 轴 0～7：`ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y`。
- 按钮 0～7：A、B、X、Y、LB、RB、Back、Start。
- LT/RT 未按时为负值，按下时为正值；摇杆范围为 -32767～32767。
- 程序创建设备后通过 joydev ioctl 验证实际轴/按钮编号，不匹配时退出。

不创建虚拟设备、只打印事件的调试模式无需 root：

```bash
./deploy/sim_joystick/build/sim_joystick --dry-run
```

该模式不会让机器人收到输入。终端交互测试可执行：

```bash
python3 deploy/sim_joystick/test_terminal.py deploy/sim_joystick/build/sim_joystick
ctest --test-dir deploy/sim_joystick/build --output-on-failure
```

如果没有生成 `/dev/input/jsN`，检查 `joydev` 是否加载，以及内核是否启用了
`CONFIG_INPUT_JOYDEV`。如果 MuJoCo 报 `Joystick open failed`，检查设备路径、
读取权限，并确认虚拟手柄程序仍在运行。

uinput 接口参考：[Linux kernel documentation](https://www.kernel.org/doc/html/latest/input/uinput.html)。

旧版本若启动时报 `Kernel joystick layout does not match MuJoCo Xbox mapping.`，
可能是将映射查询 ioctl 的正数成功返回值误判为失败。当前版本已修复：
ioctl 返回负数才表示失败，真正的布局不匹配会打印实际与预期编号。
更新代码后重新执行上面的编译命令，再启动程序；无需为此修改 MuJoCo 配置。
