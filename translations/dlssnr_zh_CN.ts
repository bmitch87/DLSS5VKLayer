<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN">
<context>
    <name>MainWindow</name>
    <message>
        <location filename="../gui/mainwindow.cpp" line="263"/>
        <source>DLSS5VKLayer Helper</source>
        <translation>DLSS5VKLayer Helper</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="306"/>
        <source>Browse...</source>
        <translation>浏览…</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="315"/>
        <source>Proton or Wine for the helper process, which is a Windows executable
because the model is a Windows DLL.
This is not what the game runs under -- native Linux games work with this set too.</source>
        <translation>助手进程所用的 Proton 或 Wine，因为助手是 Windows 可执行文件
而模型是 Windows DLL。
这不是游戏运行的环境——原生 Linux 游戏也用这一设置。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="319"/>
        <source>Runner</source>
        <translation>运行器</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="320"/>
        <source>Path</source>
        <translation>路径</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="324"/>
        <source>Start helper</source>
        <translation>启动助手</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="325"/>
        <source>Stop helper</source>
        <translation>停止助手</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="327"/>
        <source>Select a saved profile to load, or choose &apos;(default)&apos; to reset.</source>
        <translation>选择要载入的已保存配置档案，或选择“（默认）”以重置。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="330"/>
        <source>Settings changed since this selection was loaded. Click to reload it.</source>
        <translation>载入该选项之后设置已改动。点击可重新载入。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="331"/>
        <source>Save</source>
        <translation>保存</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="332"/>
        <source>Save current settings to the selected profile.</source>
        <translation>把当前设置保存到选中的配置档案。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="374"/>
        <source>Settings</source>
        <translation>设置</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="385"/>
        <source>Rebuild spacing (ms)</source>
        <translation>重建间隔（毫秒）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="389"/>
        <source>How long the helper waits after a model setting changes before it rebuilds the pass,
and between one rebuild and the next.
Rebuilding is expensive, and back-to-back rebuilds have been seen to wedge the model
on some drivers.
Lower is snappier; 0 rebuilds immediately and chains the rest back to back.
Raise it if the model ever stops answering after changing settings.</source>
        <translation>模型设置改动后，助手等待多久才重建 Pass，
以及两次重建之间的间隔。
重建开销很大，而且已有连续重建导致模型
在某些驱动上卡死的案例。
数值越低越跟手；0 表示立即重建，其余重建会接连进行。
如果改动设置后模型不再响应，就把它调高。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="402"/>
        <source>Reset all settings...</source>
        <translation>重置所有设置…</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="404"/>
        <source>Open helper log</source>
        <translation>打开助手日志</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="457"/>
        <source>NGX binaries</source>
        <translation>NGX 二进制文件</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="461"/>
        <source>Where the NVIDIA NGX DLLs live. The helper loads nvngx_dlssnr.dll from here.
These are NVIDIA&apos;s proprietary files and are not shipped with this package -- import your own.</source>
        <translation>NVIDIA NGX DLL 的存放位置。助手从这里加载 nvngx_dlssnr.dll。
这些是 NVIDIA 的专有文件，本包不附带——请自行导入。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="463"/>
        <source>Import binaries...</source>
        <translation>导入二进制文件…</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="465"/>
        <source>Copy the NVIDIA NGX DLLs from a folder you choose into the binaries folder.
Pick the folder that holds nvngx_dlssnr.dll (and nvngx.dll, nvapi64.dll, sl.*.dll if you have them).
Restart the helper afterwards so it loads the new files.</source>
        <translation>把你选择的文件夹里的 NVIDIA NGX DLL 复制到二进制文件目录。
请选存放 nvngx_dlssnr.dll 的文件夹（如果还有 nvngx.dll、nvapi64.dll、sl.*.dll 就一并选中）。
之后重启助手，让它加载新文件。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="469"/>
        <source>Open binaries folder</source>
        <translation>打开二进制文件目录</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="470"/>
        <source>Open the folder the helper loads the NGX DLLs from.</source>
        <translation>打开助手加载 NGX DLL 的目录。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="501"/>
        <source>Select runner</source>
        <translation>选择运行器</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="564"/>
        <source>Delete profile</source>
        <translation>删除配置档案</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="565"/>
        <source>Delete profile &quot;%1&quot;?</source>
        <translation>删除配置档案“%1”？</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="570"/>
        <location filename="../gui/mainwindow.cpp" line="840"/>
        <location filename="../gui/mainwindow.cpp" line="899"/>
        <location filename="../gui/mainwindow.cpp" line="942"/>
        <location filename="../gui/mainwindow.cpp" line="974"/>
        <location filename="../gui/mainwindow.cpp" line="980"/>
        <location filename="../gui/mainwindow.cpp" line="1104"/>
        <location filename="../gui/mainwindow.cpp" line="1108"/>
        <location filename="../gui/mainwindow.cpp" line="1144"/>
        <location filename="../gui/mainwindow.cpp" line="1168"/>
        <location filename="../gui/mainwindow.cpp" line="1189"/>
        <location filename="../gui/mainwindow.cpp" line="1191"/>
        <source>DLSS5VKLayer</source>
        <translation>DLSS5VKLayer</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="571"/>
        <source>Could not delete:\n</source>
        <translation>无法删除：\n</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="841"/>
        <source>Reset every setting to its default? The helper will rebuild its features from the defaults.</source>
        <translation>把所有设置恢复为默认值？助手将按默认值重建其特性。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="873"/>
        <source>(default)</source>
        <translation>（默认）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="899"/>
        <location filename="../gui/mainwindow.cpp" line="974"/>
        <source>Shared memory not attached yet.</source>
        <translation>共享内存尚未连接。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="908"/>
        <source>Save profile</source>
        <translation>保存配置档案</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="909"/>
        <source>Profile name:</source>
        <translation>配置档案名称：</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="943"/>
        <source>Could not write to:
</source>
        <translation>无法写入：
</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="981"/>
        <source>Could not read:
</source>
        <translation>无法读取：
</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1037"/>
        <source>System Wine</source>
        <translation>系统 Wine</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1052"/>
        <source>Custom: </source>
        <translation>自定义：</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1104"/>
        <source>dlssnr-helper CLI not found.</source>
        <translation>未找到 dlssnr-helper 命令行工具。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1108"/>
        <source>Could not open shared memory file.</source>
        <translation>无法打开共享内存文件。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1139"/>
        <source>Select the folder holding the NVIDIA NGX DLLs</source>
        <translation>选择存放 NVIDIA NGX DLL 的文件夹</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1145"/>
        <source>Could not create the binaries folder:
</source>
        <translation>无法创建二进制文件目录：
</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1169"/>
        <source>No NVIDIA NGX DLLs were found in:
</source>
        <translation>在以下位置未找到 NVIDIA NGX DLL：
</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1170"/>
        <source>

Pick the folder that actually contains nvngx_dlssnr.dll.</source>
        <translation>

请选择真正包含 nvngx_dlssnr.dll 的文件夹。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1179"/>
        <source>Imported into:
</source>
        <translation>已导入到：
</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1180"/>
        <source>

Could not copy:
</source>
        <translation>

无法复制：
</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1182"/>
        <source>

nvngx_dlssnr.dll was not among them -- neural processing stays disabled.</source>
        <translation>

其中没有 nvngx_dlssnr.dll——神经处理将保持关闭。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1184"/>
        <source>

The helper is running; restart it to load the new files.</source>
        <translation>

助手正在运行；请重启它以加载新文件。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1186"/>
        <source>

Start the helper to use them.</source>
        <translation>

启动助手即可使用它们。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1214"/>
        <source>NVIDIA NGX binaries not found</source>
        <translation>未找到 NVIDIA NGX 二进制文件</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1225"/>
        <source>The neural model&apos;s DLL (nvngx_dlssnr.dll) is not in the binaries folder, so neural processing stays off and games present their own frames.</source>
        <translation>神经模型的 DLL（nvngx_dlssnr.dll）不在二进制文件目录中，因此神经处理保持关闭，游戏会呈现自己的帧。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1229"/>
        <source>Import it from a folder you own, or open the folder to place the files yourself:
</source>
        <translation>从你自己的文件夹导入，或打开该目录自行放入文件：
</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1230"/>
        <source>

These are NVIDIA&apos;s proprietary files and are not shipped with this package.</source>
        <translation>

这些是 NVIDIA 的专有文件，本包不附带。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1239"/>
        <source>Import...</source>
        <translation>导入…</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1240"/>
        <source>Open folder</source>
        <translation>打开目录</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="764"/>
        <location filename="../gui/mainwindow.cpp" line="1241"/>
        <source>Later</source>
        <translation>稍后</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="413"/>
        <location filename="../gui/mainwindow.cpp" line="760"/>
        <source>Language</source>
        <translation>语言</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="423"/>
        <source>Follow the system language</source>
        <translation>跟随系统语言</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="424"/>
        <source>Use whichever language the session runs in.</source>
        <translation>使用系统会话当前的语言。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="438"/>
        <source>%1 (translation not installed)</source>
        <translation>%1（未安装该语言的翻译）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="443"/>
        <source>Overridden by DLSSNR_LANG=%1 for this session</source>
        <translation>本次运行已被环境变量 DLSSNR_LANG=%1 覆盖</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="761"/>
        <source>The interface language changes the next time the window opens.</source>
        <translation>界面语言将在窗口下次打开时生效。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="763"/>
        <source>Restart now</source>
        <translation>立即重启</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1283"/>
        <source>shared memory not attached</source>
        <translation>共享内存未连接</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1287"/>
        <source>starting</source>
        <translation>正在启动</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1287"/>
        <source>no Vulkan device</source>
        <translation>无 Vulkan 设备</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1287"/>
        <source>no NGX binaries</source>
        <translation>无 NGX 二进制文件</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1288"/>
        <source>the model would not start</source>
        <translation>模型无法启动</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1288"/>
        <source>running</source>
        <translation>运行中</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1288"/>
        <source>stopped</source>
        <translation>已停止</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1290"/>
        <source>unknown</source>
        <translation>未知</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1323"/>
        <source>&lt;span style=&quot;color:#e53935;&quot;&gt;Shared memory is v%1, this build is v%2 -- the layer or helper is out of date. Update them together.&lt;/span&gt;</source>
        <translation>&lt;span style=&quot;color:#e53935;&quot;&gt;共享内存为 v%1，本构建为 v%2——层或助手版本过旧。请一起更新。&lt;/span&gt;</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1331"/>
        <source>&lt;span style=&quot;color:#43a047;&quot;&gt;&amp;#9679; Active&lt;/span&gt;</source>
        <translation>&lt;span style=&quot;color:#43a047;&quot;&gt;&amp;#9679; 活动&lt;/span&gt;</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1332"/>
        <source>&lt;span style=&quot;color:#fb8c00;&quot;&gt;&amp;#9679; Idle&lt;/span&gt;</source>
        <translation>&lt;span style=&quot;color:#fb8c00;&quot;&gt;&amp;#9679; 空闲&lt;/span&gt;</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1333"/>
        <source>&lt;span style=&quot;color:#9e9e9e;&quot;&gt;&amp;#9675; Inactive&lt;/span&gt;</source>
        <translation>&lt;span style=&quot;color:#9e9e9e;&quot;&gt;&amp;#9675; 未激活&lt;/span&gt;</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1334"/>
        <source>Helper: %1&amp;nbsp;&amp;nbsp;&amp;nbsp;%2</source>
        <translation>助手：%1&amp;nbsp;&amp;nbsp;&amp;nbsp;%2</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1383"/>
        <source>Rendering</source>
        <translation>渲染</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1387"/>
        <source>Neural rendering</source>
        <translation>神经渲染</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1388"/>
        <location filename="../gui/mainwindow.cpp" line="1482"/>
        <source>Enabled</source>
        <translation>启用</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1389"/>
        <source>Run the model at all. Off leaves the game&apos;s own frame untouched.</source>
        <translation>是否运行模型。关闭时游戏自己的帧不受影响。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1390"/>
        <source>Style</source>
        <translation>风格</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1390"/>
        <source>Default</source>
        <translation>默认</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1390"/>
        <source>Natural</source>
        <translation>自然</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1390"/>
        <source>Cinematic</source>
        <translation>电影感</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1391"/>
        <source>The model&apos;s own processing profiles.</source>
        <translation>模型自带的处理配置档案。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1392"/>
        <source>Preset</source>
        <translation>预设</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1392"/>
        <source>The model&apos;s own render preset.</source>
        <translation>模型自带的渲染预设。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1394"/>
        <source>Intensity</source>
        <translation>强度</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1395"/>
        <source>How hard the model works.</source>
        <translation>模型的工作强度。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1396"/>
        <source>Local structure</source>
        <translation>局部结构</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1398"/>
        <source>Local tone</source>
        <translation>局部色调</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1400"/>
        <source>Skin structure</source>
        <translation>皮肤结构</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1401"/>
        <source>-1 follows local structure, which is the model&apos;s own default. It is not a strength of zero.</source>
        <translation>-1 表示跟随局部结构，这是模型自身的默认值。它并不代表强度为零。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1404"/>
        <source>Auto skin mask</source>
        <translation>自动皮肤遮罩</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1404"/>
        <source>The model&apos;s automatic skin mask.</source>
        <translation>模型自带的自动皮肤遮罩。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1406"/>
        <source>Sharpness</source>
        <translation>锐度</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1407"/>
        <source>The one strength the model reads every frame, so it takes effect at once.</source>
        <translation>这是模型每帧都会读取的唯一强度项，因此立即生效。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1410"/>
        <source>Cost</source>
        <translation>开销</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1411"/>
        <source>Passes</source>
        <translation>Pass 数量</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1412"/>
        <source>How many times the model runs over one frame, each pass shown the last one&apos;s answer.
Every pass is another full run of the model and another feature holding its own history, so the cost is close to linear.</source>
        <translation>模型对一帧运行多少次，每个 Pass 都能看到上一个 Pass 的结果。
每个 Pass 都是模型的一次完整运行，也各有一份自己的历史，因此开销接近线性增长。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1417"/>
        <source>Model resolution</source>
        <translation>模型分辨率</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1418"/>
        <source>What fraction of the frame the model works at. The frame itself is never reduced.
Below 100% also cuts what crosses shared memory, quadratically.
Above 100% the model supersamples, which on this transport is expensive: at 200% on a 4K frame it is 132 MB each way, every frame.</source>
        <translation>模型以多大的比例处理画面。画面本身从不缩小。
低于 100% 还能以平方关系减少跨共享内存的数据量。
高于 100% 时模型会超采样，在这条传输路径上代价高昂：4K 画面下 200% 时每帧单向就是 132 MB。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1423"/>
        <source>Down-leg filter</source>
        <translation>降采样滤波</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1424"/>
        <source>(fsr1, unsupported)</source>
        <translation>（fsr1，不支持）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1424"/>
        <source>Bicubic</source>
        <translation>Bicubic</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1424"/>
        <source>Catmull-Rom</source>
        <translation>Catmull-Rom</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1424"/>
        <source>Lanczos2</source>
        <translation>Lanczos2</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1424"/>
        <source>Lanczos3</source>
        <translation>Lanczos3</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1425"/>
        <source>Kaiser2</source>
        <translation>Kaiser2</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1425"/>
        <source>Kaiser3</source>
        <translation>Kaiser3</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1425"/>
        <source>Magic</source>
        <translation>Magic</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1426"/>
        <source>How a supersampled answer is averaged back to the frame&apos;s size. Only used above 100%.</source>
        <translation>超采样的结果如何平均回画面尺寸。仅在高于 100% 时使用。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1428"/>
        <source>Per-pass settings...</source>
        <translation>各 Pass 设置…</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1432"/>
        <location filename="../gui/mainwindow.cpp" line="1442"/>
        <source>Quality</source>
        <translation>质量</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1434"/>
        <source>Motion</source>
        <translation>运动</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1435"/>
        <source>Estimate motion vectors</source>
        <translation>估计运动矢量</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1436"/>
        <source>The model reasons about what moved between frames.
A layer at present time has no motion vectors from the engine, so they are estimated on the GPU&apos;s optical-flow engine from the two frames the helper already has.
Off hands the model a zero field, which is what it used to get.</source>
        <translation>模型据此推断帧间有什么在移动。
在呈现时刻工作的层拿不到引擎提供的运动矢量，因此改用 GPU 的光流引擎，从助手已有的两帧中估计。
关闭时交给模型的是零矢量场，也就是它过去一直拿到的结果。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1441"/>
        <source>Motion quality</source>
        <translation>运动质量</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1442"/>
        <source>Fast</source>
        <translation>快速</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1442"/>
        <source>Balanced</source>
        <translation>均衡</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1443"/>
        <source>How much of the frame&apos;s budget the flow estimate may take.</source>
        <translation>光流估计最多能占用多少帧预算。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1444"/>
        <source>Motion units</source>
        <translation>运动单位</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1445"/>
        <source>Normalised</source>
        <translation>归一化</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1445"/>
        <source>Pixels</source>
        <translation>像素</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1445"/>
        <source>UV 0..1</source>
        <translation>UV 0..1</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1446"/>
        <source>What the numbers in the field mean to the model.
Pixels is what the estimate produces; the others are for matching a model that expects them.</source>
        <translation>矢量场中的数值对模型的含义。
“像素”是估计本身产生的单位；其余两项用于匹配期望这种单位的模型。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1449"/>
        <source>Motion pixel size</source>
        <translation>运动像素尺寸</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1450"/>
        <source>1 px</source>
        <translation>1 像素</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1450"/>
        <source>2 px</source>
        <translation>2 像素</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1450"/>
        <source>4 px</source>
        <translation>4 像素</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1450"/>
        <source>8 px</source>
        <translation>8 像素</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1451"/>
        <source>The optical-flow grid spacing in source-image pixels. Unsupported grids fall back to the nearest grid the GPU can use.</source>
        <translation>光流网格的间距，以源图像像素为单位。不受支持的网格会回退到 GPU 可用的最接近网格。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1455"/>
        <source>Input and precision</source>
        <translation>输入与精度</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1456"/>
        <source>HDR input</source>
        <translation>HDR 输入</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1457"/>
        <location filename="../gui/mainwindow.cpp" line="1575"/>
        <source>Auto</source>
        <translation>自动</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1457"/>
        <location filename="../gui/mainwindow.cpp" line="1610"/>
        <location filename="../gui/mainwindow.cpp" line="1621"/>
        <source>Off</source>
        <translation>关闭</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1457"/>
        <source>Force float16</source>
        <translation>强制 float16</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1458"/>
        <source>Let the model see the frame&apos;s real light instead of a tone-mapped copy.
Auto turns it on when the swapchain is HDR -- a float swapchain, or 10-bit with a PQ colour space -- and the proxy then crosses as float16 carrying linear light, PQ-decoded first when the swapchain carries PQ.
Off keeps the 8-bit proxy whatever the game presents.
Force feeds the float proxy to an SDR swapchain too, which is an A/B tool rather than a preference.
The model has the last word: if it refuses float input the pass falls back to 8-bit on its own.</source>
        <translation>让模型看到画面的真实光照，而不是色调映射后的副本。
当交换链是 HDR——浮点交换链，或带 PQ 色彩空间的 10 位——时，“自动”会启用它，代理随后以 float16 传输线性光；若交换链带 PQ，则先做 PQ 解码。
“关闭”时无论游戏呈现什么，都保持 8 位代理。
“强制”会连 SDR 交换链也喂入浮点代理，这是 A/B 对比工具，而非偏好设置。
模型有最终决定权：如果它拒绝浮点输入，Pass 会自行回退到 8 位。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1467"/>
        <source>16-bit SDR intermediates</source>
        <translation>16 位 SDR 中间图像</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1468"/>
        <source>Keep the images between SDR model passes at 16-bit. Disable to keep them 8-bit and reduce VRAM and GPU bandwidth use; HDR is always float16.</source>
        <translation>让 SDR 模型各 Pass 之间的图像保持 16 位。禁用后保持 8 位，可减少 VRAM 与 GPU 带宽占用；HDR 始终为 float16。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1473"/>
        <location filename="../gui/mainwindow.cpp" line="1479"/>
        <source>Composition</source>
        <translation>合成</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1483"/>
        <source>Off: the model&apos;s raw answer is presented as the frame.
On: the answer is blended onto the frame under the limits below, which are hidden while this is off.</source>
        <translation>关闭：直接把模型的原始结果作为画面呈现。
开启：结果会按下面的限制混合到画面上；关闭时这些限制会被隐藏。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1488"/>
        <source>Detail strength</source>
        <translation>细节强度</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1490"/>
        <source>How much of the model&apos;s edit reaches the frame.
At zero the frame is bit-identical to the game&apos;s own.</source>
        <translation>模型的修改有多少会作用到画面上。
为零时画面与游戏自身的画面逐位相同。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1492"/>
        <source>Color strength</source>
        <translation>色彩强度</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1494"/>
        <source>How much of the model&apos;s color comes with its light. At zero the frame keeps the game&apos;s hue exactly.</source>
        <translation>模型的光照中有多少色彩一并带过来。为零时画面完全保留游戏的色相。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1496"/>
        <source>Color bound (%)</source>
        <translation>色彩界限（%）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1498"/>
        <source>How far the model may move a pixel&apos;s color away from the game&apos;s own.

The model disagrees about color most at edges, and taking its hue whole there can put one color on one side of an edge and its complement on the other. But a large disagreement is also what a real correction looks like -- a strip light the game glows blue and the model returns white -- so a rule that backs off as disagreement grows throws away the verdict exactly where there is one.

This bounds the move instead of refusing it. A correction of ordinary size passes through whole; anything several times larger is capped, keeping its direction and losing only its length, so it can never invert.

200 is the default. Lower it if colored fringes appear along high-contrast edges; 0 keeps the game&apos;s hue exactly, which is the same as setting Color strength to 0.</source>
        <translation>模型可以把一个像素的颜色偏离游戏自身的颜色多远。

模型在边缘处对颜色的分歧最大，在那里整体采用它的色相，可能让边缘一侧是某种颜色、另一侧是它的补色。但大幅分歧也正是一次真实修正的样子——游戏里发蓝的灯带，模型返回白色——所以一条随分歧增大而后退的规则，恰恰会在真正有结论的地方把结论丢掉。

这里改为限制移动幅度，而不是拒绝移动。常规大小的修正会完整通过；大出数倍的修正会被截断，方向保留、只损失长度，因此绝不会反转。

200 是默认值。如果高对比边缘出现彩色镶边就调低；0 表示完全保留游戏的色相，等同于把“色彩强度”设为 0。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1511"/>
        <source>Smooth the relighting (%)</source>
        <translation>平滑重光照（%）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1513"/>
        <source>What lets the Highlight guard be raised without the picture going patchy.

Classic and Matched residual rebuild the frame as its own pixel times one number. Where the model and the frame agree that number is 1 and nothing happens, which is why flat surfaces are always clean. On detailed content the model&apos;s answer differs sharply from pixel to pixel -- that difference is the enhancement -- so the number varies fast, and the guard is the only thing holding it. Raise the guard and that variation lands as blown and black pixels wearing whatever colour the texture had.

This takes the number from the pixel&apos;s neighbourhood instead of the pixel. What the model knows at this scale is how much light belongs here, not which pixel is brighter than its neighbour -- the frame already knows that, and is what gets multiplied.

0 is the old per-pixel behaviour. Raise it toward 100 if you want a high guard: the relighting keeps its full range and stops speckling.</source>
        <translation>它让“高光保护”可以调高而不至于让画面出现斑块。

“经典”和“匹配残差”把画面重建为“自身像素乘以一个数”。模型与画面一致的地方这个数是 1，什么也不发生，所以平坦表面总是干净的。在细节丰富的内容上，模型的结果逐像素差异很大——这个差异正是增强所在——于是这个数变化剧烈，而保护值就是唯一约束它的东西。调高保护值，这种变化就会以过曝和死黑像素的形式出现，还带着纹理原本的颜色。

这里改为从像素的邻域而不是像素本身取这个数。模型在这个尺度上知道的是“这里该有多少光”，而不是“哪个像素比邻居更亮”——后者画面自己就知道，而且正是被乘的对象。

0 是旧的逐像素行为。想要高保护值就把它往 100 调：重光照保留完整范围，同时不再出现斑点。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1529"/>
        <source>Highlight guard</source>
        <translation>高光保护</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1531"/>
        <source>How far the pass may move the light. A detail pass has no business restyling a light source, whatever the model returns.

This sets how far the light over a pixel&apos;s neighbourhood may move, which is the relighting you are asking for when you raise it. How far a single pixel may then depart from its own neighbourhood is a separate, fixed bound that does not move with this -- so raising the guard buys range without buying speckle. It used to be one number doing both jobs, and at a high setting the per-pixel half stopped bounding anything, which is what put blown and black pixels on detailed surfaces.

Detail is a pixel differing from its neighbours by tens of percent. A blowout is one differing by multiples. Only the second is refused.</source>
        <translation>Pass 可以把光照移动多远。细节 Pass 无论如何都不该把光源重新调色，不管模型返回什么。

这里设定的是像素邻域上的光照可以移动多远，也就是你调高它时想要的重光照幅度。单个像素随后可以偏离自身邻域多远，则是一个独立且固定的界限，不随这个值变化——所以调高保护值买到的是范围，而不是斑点。过去是同一个数兼顾两件事，设得高时逐像素的那一半就不再约束任何东西，于是细节表面上出现了过曝和死黑像素。

细节是像素与邻居相差百分之几十。过曝则是相差数倍。只有后者会被拒绝。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1545"/>
        <source>How the answer is applied</source>
        <translation>结果的合成方式</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1546"/>
        <source>Classic</source>
        <translation>经典</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1546"/>
        <source>Matched residual</source>
        <translation>匹配残差</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1546"/>
        <source>Native + edit</source>
        <translation>原生 + 编辑</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1547"/>
        <source>How a model that worked at a different size from the frame is brought back. Active at any working scale other than 1 -- above it as well as below, since what matters is only that the model&apos;s raster differs from the frame.

Classic and Matched residual both rebuild the output by scaling the frame&apos;s own pixel by a per-pixel luminance ratio between the model&apos;s answer and the frame. Where the two agree that ratio is 1 and nothing happens, which is why flat surfaces are always clean. On detailed content the model&apos;s answer differs from the frame a great deal from one pixel to the next -- that difference is the enhancement -- so the ratio becomes large and varies sharply, and the Highlight guard below then bounds it. Raising that guard lets more of the variation through and it shows as patches of over- and under-bright texture on exactly the detailed things you were trying to enhance.

Native + edit has no ratio at all. The frame&apos;s own pixels are the result and only the model&apos;s difference is added to them, so geometry, text and edges the model left alone stay at native sharpness -- and there is no per-pixel division to blow up. Use this one if raising the Highlight guard makes detailed surfaces go patchy.</source>
        <translation>模型工作尺寸与画面不同时，结果如何还原。只要工作比例不是 1 就生效——无论高于还是低于 1，因为关键在于模型的栅格与画面不同。

“经典”和“匹配残差”都用模型结果与画面之间逐像素的亮度比去缩放画面自身的像素，从而重建输出。两者一致的地方比值是 1，什么也不发生，所以平坦表面总是干净的。在细节丰富的内容上，模型结果与画面逐像素差异很大——这个差异正是增强所在——于是比值变大且变化剧烈，下面的“高光保护”会对它加以限制。调高保护值会让更多变化通过，表现为忽亮忽暗的纹理块，而且恰恰出现在你想增强的细节上。

“原生 + 编辑”完全没有比值。画面自身的像素就是结果，只把模型的差值加在上面，因此模型未改动的几何、文字和边缘保持原生锐度——也不存在会爆掉的逐像素除法。如果调高“高光保护”后细节表面出现斑块，就用这一项。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1573"/>
        <source>Color</source>
        <translation>色彩</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1574"/>
        <source>Frame holds</source>
        <translation>帧承载内容</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1575"/>
        <source>A finished picture</source>
        <translation>已完成画面</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1575"/>
        <source>Linear light</source>
        <translation>线性光</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1576"/>
        <source>Whether the swapchain carries a frame the game already tone mapped or open-ended light.
Auto decides from the format and is right for almost every game.</source>
        <translation>交换链承载的是游戏已经做过色调映射的画面，还是没有上限的光照。
“自动”会根据格式判断，对几乎所有游戏都适用。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1579"/>
        <source>White point from</source>
        <translation>白点来源</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1580"/>
        <source>The slider below</source>
        <translation>使用下方滑块</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1580"/>
        <source>Measured off the frame</source>
        <translation>从画面中测量</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1581"/>
        <source>Only meaningful on a linear frame; a finished picture has no white point to find.</source>
        <translation>仅对线性画面有意义；已完成的画面没有白点可找。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1583"/>
        <source>Paper white</source>
        <translation>纸白</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1584"/>
        <source>What the model should treat as white, when it is not being measured.</source>
        <translation>不进行测量时，模型应将什么视为白色。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1585"/>
        <source>White point scale</source>
        <translation>白点缩放</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1586"/>
        <source>Multiplies whichever white point is in use. Higher means highlights sit lower on the curve.</source>
        <translation>乘以当前使用的白点。数值越高，高光在曲线上的位置越低。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1588"/>
        <source>Trim (measured)</source>
        <translation>修正系数（测量值）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1589"/>
        <source>Multiplies a measured white point only. Kept apart from the slider because a value found against one is meaningless against the other.</source>
        <translation>仅乘以测量得到的白点。它与滑块分开，因为针对其中一个找到的值对另一个没有意义。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1593"/>
        <location filename="../gui/mainwindow.cpp" line="1595"/>
        <source>Inspect</source>
        <translation>查看</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1596"/>
        <source>Apply the model&apos;s edit</source>
        <translation>应用模型的修改</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1597"/>
        <source>Off keeps the whole pass running and shows the clean frame, so the cost is unchanged and only the picture differs.</source>
        <translation>关闭时整个 Pass 照常运行，只显示干净的画面，因此开销不变，只有画面不同。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1599"/>
        <source>Hold frame</source>
        <translation>冻结画面</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1600"/>
        <source>Freeze the frame the pass works on, so changing a setting re-runs the model and the composition on the same picture.
The only clean way to compare two settings.</source>
        <translation>冻结 Pass 处理的画面，这样改动设置时模型与合成都会在同一张画面上重跑。
这是对比两项设置的唯一干净做法。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1603"/>
        <source>Proxy</source>
        <translation>代理</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1604"/>
        <source>Soft knee</source>
        <translation>软拐点</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1604"/>
        <source>Neutwo</source>
        <translation>Neutwo</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1604"/>
        <source>Neutwo, replace</source>
        <translation>Neutwo（替换）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1604"/>
        <source>Hybrid</source>
        <translation>混合</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1604"/>
        <source>Hybrid, replace</source>
        <translation>混合（替换）</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1605"/>
        <source>Which picture the model is shown, and whether its answer is composed onto the frame or substituted for it.
Soft knee is the default and the two replace modes are known to flash on bright lights.</source>
        <translation>给模型看哪张画面，以及它的结果是合成到画面上还是替换画面。
“软拐点”是默认值，两种“替换”模式已知会在明亮光源处闪烁。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1609"/>
        <source>Debug view</source>
        <translation>调试视图</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1610"/>
        <source>The picture the model saw</source>
        <translation>模型看到的画面</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1610"/>
        <source>Its raw answer</source>
        <translation>它的原始结果</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1610"/>
        <source>What it changed</source>
        <translation>它改动了什么</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1611"/>
        <source>Where the color bound engages</source>
        <translation>色彩界限生效的位置</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1611"/>
        <source>The color before the bound</source>
        <translation>界限前的颜色</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1612"/>
        <source>&quot;What it changed&quot; is amplified and centred on grey, so both directions of the edit are visible at once.

&quot;Where the color bound engages&quot; is green where the model&apos;s color passes whole and red where it is held back. It puts a wrong color on one side or the other of that line: green means the bound is not engaging and the fault is upstream; red means the bound is working and the color is coming from the frame&apos;s own hue times one scalar, which makes it a brightness problem rather than a color one.</source>
        <translation>“它改动了什么”会放大并以灰色为中心，因此修改的两个方向可以同时看到。

“色彩界限生效的位置”中，模型的颜色完整通过的地方是绿色，被拦下的地方是红色。它能把错误的颜色定位到这条线的某一侧：绿色表示界限没有生效，问题在上游；红色表示界限正在起作用，颜色来自画面自身色相乘以一个标量，因此这是亮度问题而不是颜色问题。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1619"/>
        <source>Debug scale</source>
        <translation>调试缩放</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1620"/>
        <source>What the debug views are multiplied by on their way out.</source>
        <translation>调试视图输出前所乘的系数。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1621"/>
        <source>Compare</source>
        <translation>对比</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1621"/>
        <source>Side by side</source>
        <translation>并排</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1621"/>
        <source>Wipe</source>
        <translation>擦除</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1622"/>
        <source>Shows the pass against itself.
The wipe cuts one frame and resamples nothing, so it is the one to play with.
Works with composition off too: the model&apos;s raw answer is then shown against the frame.</source>
        <translation>把 Pass 与自身做对比。
“擦除”只是切开一帧、不做任何重采样，所以最适合拿来玩。
合成关闭时也能用：这时显示的是模型原始结果与画面的对比。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1627"/>
        <source>Split</source>
        <translation>分割位置</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1628"/>
        <source>Zoom</source>
        <translation>缩放</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1629"/>
        <source>Side by side only. 1 fits the whole frame and accepts the bars; 2 fills the half and crops.</source>
        <translation>仅用于并排模式。1 时整帧完整显示，接受黑边；2 时填满半幅并裁剪。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1631"/>
        <source>Swap sides</source>
        <translation>交换两侧</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1632"/>
        <source>Which side the edited frame sits on. Worth having because the eye is not even-handed about left and right.</source>
        <translation>编辑后的画面放在哪一侧。之所以值得保留，是因为眼睛对左右并不一视同仁。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1643"/>
        <source>None</source>
        <translation>无</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1653"/>
        <source>Toggles the pass in game. Read by the layer, which works on an X11 or XWayland
session and anywhere you are in the &apos;input&apos; group.
It cannot work for a game presenting through winewayland.</source>
        <translation>在游戏内开关 Pass。由层读取，可在 X11 或 XWayland
会话以及任何你处于 &apos;input&apos; 组的环境中使用。
对通过 winewayland 呈现的游戏无法工作。</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1656"/>
        <source>Toggle key</source>
        <translation>切换热键</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1664"/>
        <source>If that key does nothing -- a Wayland game, or not in the &apos;input&apos; group -- bind this to a shortcut in your desktop&apos;s own settings instead. That works over a fullscreen game and needs no permissions:

    dlssnr-shmctl %1 toggle enabled</source>
        <translation>如果那个键没有反应——Wayland 游戏，或不在 &apos;input&apos; 组——就改为在你的桌面环境自身设置里把它绑定为快捷键。这样在全屏游戏上也能用，而且不需要任何权限：

    dlssnr-shmctl %1 toggle enabled</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1677"/>
        <source>Capture frames</source>
        <translation>抓取帧</translation>
    </message>
    <message>
        <location filename="../gui/mainwindow.cpp" line="1679"/>
        <source>Writes that many matched before/after pairs to the state directory.
Same frames, same run, one variable.</source>
        <translation>把该数量的前后配对比图写入状态目录。
相同的帧、同一次运行，只有一项变量。</translation>
    </message>
</context>
<context>
    <name>PassDialog</name>
    <message>
        <location filename="../gui/passdialog.cpp" line="16"/>
        <source>Per-pass model settings</source>
        <translation>各 Pass 的模型设置</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="21"/>
        <source>A pass follows the global settings for anything it does not tick. These are read when that pass&apos;s feature is built, so a change takes a moment to appear.</source>
        <translation>Pass 对未勾选的项沿用全局设置。这些值在该 Pass 的特性构建时读取，因此改动需要片刻才会生效。</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="33"/>
        <source>Pass %1</source>
        <translation>Pass %1</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="80"/>
        <source>Intensity</source>
        <translation>强度</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="84"/>
        <source>Local structure</source>
        <translation>局部结构</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="88"/>
        <source>Local tone</source>
        <translation>局部色调</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="92"/>
        <source>Skin structure</source>
        <translation>皮肤结构</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="96"/>
        <source>Sharpness</source>
        <translation>锐度</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="100"/>
        <source>Default</source>
        <translation>默认</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="100"/>
        <source>Natural</source>
        <translation>自然</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="100"/>
        <source>Cinematic</source>
        <translation>电影感</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="104"/>
        <source>Style</source>
        <translation>风格</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="111"/>
        <source>Preset</source>
        <translation>预设</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="113"/>
        <source>on</source>
        <translation>启用</translation>
    </message>
    <message>
        <location filename="../gui/passdialog.cpp" line="116"/>
        <source>Auto skin mask</source>
        <translation>自动皮肤遮罩</translation>
    </message>
</context>
</TS>
