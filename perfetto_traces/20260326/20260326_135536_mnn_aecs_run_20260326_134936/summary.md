| threads | prefill ids | decode ids | scheduler | prefill tok/s | decode tok/s | log | trace |
| ---: | --- | --- | --- | ---: | ---: | --- | --- |
| 2 | 6,7 | 6,7 | dynamic ps=auto ds=auto pc=auto dc=auto | 302.516 ± 0.917 | 54.655 ± 0.426 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_135536_mnn_aecs_run_20260326_134936/logs/135551_2T_P6_7_D6_7_poldynamic_psauto_dsauto_pcauto_dcauto.log | - |
| 2 | 6,7 | 6,7 | hybrid ps=0.05 ds=0.02 pc=auto dc=auto | 302.867 ± 0.114 | 53.640 ± 0.327 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_135536_mnn_aecs_run_20260326_134936/logs/135553_2T_P6_7_D6_7_polhybrid_ps0p05_ds0p02_pcauto_dcauto.log | - |
| 2 | 6,7 | 6,7 | guided ps=auto ds=auto pc=auto dc=auto | 303.690 ± 0.296 | 53.925 ± 0.455 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/20260326_135536_mnn_aecs_run_20260326_134936/logs/135556_2T_P6_7_D6_7_polguided_psauto_dsauto_pcauto_dcauto.log | - |
