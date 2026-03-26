| threads | prefill ids | decode ids | scheduler | prefill tok/s | decode tok/s | log | trace |
| ---: | --- | --- | --- | ---: | ---: | --- | --- |
| 2 | 6,7 | 6,7 | dynamic ps=auto ds=auto pc=auto dc=auto | 263.425 ± 0.184 | 50.978 ± 0.201 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_140142_mnn_aecs_run_20260326_134936/logs/140156_2T_P6_7_D6_7_poldynamic_psauto_dsauto_pcauto_dcauto.log | - |
| 2 | 6,7 | 6,7 | hybrid ps=0.05 ds=0.02 pc=auto dc=auto | 259.698 ± 0.701 | 49.597 ± 0.194 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_140142_mnn_aecs_run_20260326_134936/logs/140217_2T_P6_7_D6_7_polhybrid_ps0p05_ds0p02_pcauto_dcauto.log | - |
| 2 | 6,7 | 6,7 | guided ps=auto ds=auto pc=auto dc=auto | 290.268 ± 0.285 | 49.654 ± 0.111 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_140142_mnn_aecs_run_20260326_134936/logs/140235_2T_P6_7_D6_7_polguided_psauto_dsauto_pcauto_dcauto.log | - |
