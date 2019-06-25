# mod_vad
a freeswitch mod

# 安装
### 安装依赖
**使用webrtc+ns的开源库进行vad的检测，所以需要先编译安装vad功能对应的动态库（`libnsvad.so`）**
1. 安装依赖  
`sh install-dep.sh`
2. 编译安装动态库  
`sh install-lib.sh`

### 安装mod_vad
**安装mod_vad之前必须要先安装依赖**  
`sh install-mod.sh`

# 使用(APP)
启动：`vad start`  
关闭：`vad stop`

# 配置说明

配置文件`/usr/local/freeswitch/conf/autoload_configs/vad.conf.xml`
```xml
<param name="vad_agn" value="3"/>                       vad检测模式
<param name="vad_frame_time_width" value="10"/>         vad检测步长
<param name="vad_correlate" value="2"/>                 handle是否复用
<param name="ns_agn" value="3"/>                        ns降噪模式
<param name="check_frame_time_width" value="600"/>      检测宽度，必须是100的整数倍
<param name="slide_window_time_width" value="200"/>     滑动窗口，必须是100的整数倍，可以被检测宽度整除
<param name="start_talking_ratio" value="70"/>          开始说话的比率/100
<param name="stop_talking_ratio" value="70"/>           结束说话的比率/100
<param name="pre_media_len" value="2"/>                 vad校正（滑动窗口个数）
```

# 注 #
1. 依赖库默认会安装在`/usr/local/freeswitch/lib/vad`下
2. `libnsvad.so`会默认安装在`/usr/local/freeswitch/lib`下
3. `mod_vad.so`会默认安装在`/usr/local/freeswitch/mod`下