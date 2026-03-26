| threads | prefill ids | decode ids | scheduler | prefill tok/s | decode tok/s | log | trace |
| ---: | --- | --- | --- | ---: | ---: | --- | --- |
| 6 | 2,3,4,5,6,7 | 7 | dynamic ps=auto ds=auto pc=auto dc=auto | 506.506 ± 0.586 | 44.123 ± 0.179 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/bestbind_dynamic_baseline/logs/144803_6T_P2_3_4_5_6_7_D7_poldynamic_psauto_dsauto_pcauto_dcauto.log | - |
| 6 | 2,3,4,5,6,7 | 6,7 | dynamic ps=auto ds=auto pc=auto dc=auto | 507.220 ± 0.690 | 39.415 ± 0.148 | /home/hefeng/MNN_WSL2/perfetto_traces/20260326/bestbind_dynamic_baseline/logs/144822_6T_P2_3_4_5_6_7_D6_7_poldynamic_psauto_dsauto_pcauto_dcauto.log | - |
