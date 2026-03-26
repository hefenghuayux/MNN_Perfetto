| threads | prefill ids | decode ids | scheduler | prefill tok/s | decode tok/s | log | trace |
| ---: | --- | --- | --- | ---: | ---: | --- | --- |
| 4 | 2,3,4,7 | 2,3,4,7 | dynamic ps=auto ds=auto pc=auto dc=auto | 370.778 ± 0.156 | 51.954 ± 0.143 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_135655_mnn_aecs_run_20260326_134936/logs/135708_4T_P2_3_4_7_D2_3_4_7_poldynamic_psauto_dsauto_pcauto_dcauto.log | - |
| 4 | 2,3,4,7 | 2,3,4,7 | hybrid ps=0.05 ds=0.02 pc=auto dc=auto | 370.378 ± 0.247 | 51.394 ± 0.021 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_135655_mnn_aecs_run_20260326_134936/logs/135724_4T_P2_3_4_7_D2_3_4_7_polhybrid_ps0p05_ds0p02_pcauto_dcauto.log | - |
| 4 | 2,3,4,7 | 2,3,4,7 | guided ps=auto ds=auto pc=auto dc=auto | 370.161 ± 0.221 | 52.909 ± 0.350 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_135655_mnn_aecs_run_20260326_134936/logs/135743_4T_P2_3_4_7_D2_3_4_7_polguided_psauto_dsauto_pcauto_dcauto.log | - |
