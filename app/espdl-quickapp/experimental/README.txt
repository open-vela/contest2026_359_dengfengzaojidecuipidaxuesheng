实验性 Zipformer 14M C 运行时（2026-10-31）

这些文件当前不在 overlay Makefile 中，未改变候选 7 固件的 ASR ready 状态。
目标仍是自由中文转写；不借用云转写或固定命令词作为替代。

已实现：
- local_asr_ops：N=1 NCHW 1D/2D 分组卷积、MatMul/GEMM、逐点非线性、广播算术、Softmax、均值/求和归约。
- local_asr_graph：GLA1 固定 N=1 图导入、张量索引/转置/切片/拼接/拆分/广播、整数状态、按最后使用点回收。运行时不使用 ONNX/Python；所有常量、Q4/Q8 权重逐行读取外置文件。GLA1 计划和权重文件均是 SD 模型资源，不编进 Flash。
- local_asr_fbank：16 kHz s16、25 ms Povey 窗、10 ms 步进、512 FFT、80 维 20..7600 Hz 对数梅尔特征；去直流、0.97 预加重、无抖动噪声、反射补边。
- local_asr：完整 PCM → 特征 → encoder/decoder/joiner → 中文词表。每块输入 39 帧、推进 32 帧；保持原贪心解码、blank=0/unk=2、初始上下文 [-1,0]，并追加与参考相同的 0.8 秒静音。
- C 工作区使用同一个 glv_memory，调用者的 PCM/输出也须计入预算；libc 文件句柄开销及实板总堆仍要独立测量。失败不重试、不写 Flash、不切网络。取消/错误时不返回供下一轮回答使用的成功文字。该同步 API 只能在单一后台所有者中调用。

主机证据（workspace output/local-voice-20261031）：
- asr-arithmetic-rows-port：942 个实际节点逐节点算术对照，ASAN/UBSAN 边界及 RV32 编译。
- asr-c-graph7：完整 C 三张图，三个独立连续状态块对照；运行时实际解码 Q4/Q8 文件。encoder 工作区（包括外部输入/输出）追踪峰值约 2.82 MiB。
- asr-c-graph-lifecycle1：完整图取消、释放、文件截断及 19 个损坏计划输入；ASAN/UBSAN 通过。
- asr-c-fbank1：21 组输入，对照 kaldi-native-fbank 1.22.5，包含两段官方 16 kHz 语音、短片段、静音、脉冲、低振幅和正弦。
- asr-c-transcription1：两段官方 16 kHz 语音在完整 C 链路转写，均与同精度 sherpa 参考文字一致。主机分别约 4.49/4.23 秒，追踪峰值约 3.09 MiB；这是主机临时本地文件读取结果，不能当作 P4 / SPI SD 耗时或质量验收。
- asr-c-lifecycle1：完整识别器两轮重用、四处取消、低内存、回调失败/重入、输出不足及损坏词表；ASAN/UBSAN 通过。

量化仍是实验性输出行布局：前馈 Q4，主要卷积/注意力/decoder/joiner Q8，其余原值。三段原主机音频中第三段（8 kHz）仍有两字相对 FP32 差异；当前 C 入口只接受板上原生 16 kHz PCM，未实现 8 kHz 重采样。两段相同输出不能代替扩大中文语料、人工标注及真人识别质量评估。

还需完成：固件后台服务和唯一麦克风所有者接入、板上 SD/模型加载/中文识别/取消/内存实测、APP 语音入口与全局唤醒完整链路。通过这些后才能把 ASR ready 设为真。原始模型和已验证 SD 版本保留。

工具：compile_asr_graph.py、verify_asr_ops.py、verify_asr_graph_lifecycle.py、verify_asr_fbank.py、verify_asr_transcription.py、verify_asr_lifecycle.py（project/tools/local_voice）。模型来源和前端参考见 ASR-SOURCES.txt。
