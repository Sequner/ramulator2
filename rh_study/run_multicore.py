import os, yaml, copy, itertools
import subprocess
from calc_rh_parameters import get_rh_parameters

base_config_file = "rh_baseline.yaml"
base_config = None
with open(base_config_file, 'r') as stream:
    try:
        base_config = yaml.safe_load(stream)
    except yaml.YamlError as exc:
        print(exc)
if(base_config == None):
    print("Error: base config is None")
    exit(1)

output_path = "./results_multicore"
trace_path = "./cputraces"
trace_combination_filename = "multicore_traces.txt"

trace_combs = {}
with open(trace_combination_filename, "r") as trace_combination_file:
    for line in trace_combination_file:
        line = line.strip()
        if(line == ""):
            continue
        trace_name = line.split(",")[0]
        trace_list = line.split(",")[1:]
        trace_combs[trace_name] = trace_list

# group_list = ["HHHH", "HHHL", "HHLL", "HLLL", "LLLL"]
# num_samples_per_group = 5
# trace_name_list = [x[0] + str(x[1]) for x in itertools.product(group_list, range(num_samples_per_group))]
trace_name_list=['bzip2','gcc','milc','gromacs','cactusADM','leslie3d','namd','gobmk','dealII','soplex','hmmer','sjeng','GemsFDTD','libquantum','h264ref','lbm','omnetpp','astar','wrf','sphinx3','xalancbmk','mcf','zeusmp']
for trace_name in trace_name_list:
    for mitigation in ["Graphene", "RRS", "MithrilCache", "MithrilDDR4"]:
        for tRH in [256]:

            trace_comb = trace_combs[trace_name]

            for path in [output_path + "/" + mitigation + "/stats", output_path + "/" + mitigation + "/configs", output_path + "/" + mitigation + "/cmd_count", output_path + "/" + mitigation + "/dram_trace"]:
                if not os.path.exists(path):
                    os.makedirs(path)

            result_filename = output_path + "/" + mitigation + "/stats/" + str(tRH) + "_" + trace_name + ".txt"
            config_filename = output_path + "/" + mitigation + "/configs/" + str(tRH) + "_" + trace_name + ".yaml"
            cmd_count_filename = output_path + "/" + mitigation + "/cmd_count/" + str(tRH) + "_" + trace_name + ".cmd.count"
            dram_trace_filename = output_path + "/" + mitigation + "/dram_trace/" + str(tRH) + "_" + trace_name + ".dram.trace"
            config = copy.deepcopy(base_config)
            result_file = open(result_filename, "w")
            config_file = open(config_filename, "w")
            
            config['Frontend']['traces'] = [trace_path + "/" + trace for trace in trace_comb]
            config['MemorySystem']['Controller']['plugins'][0]['ControllerPlugin']['path'] = cmd_count_filename
            # config['MemorySystem']['Controller']['plugins'].append({'ControllerPlugin' : {'impl': 'TraceRecorder', 'path': dram_trace_filename}})
            if(mitigation == "Graphene"):
                num_table_entries, activation_threshold, reset_period_ns = get_rh_parameters(mitigation, tRH)
                config['MemorySystem']['Controller']['plugins'].append({'ControllerPlugin' : {'impl': 'Graphene', 'num_table_entries': num_table_entries, 'activation_threshold': activation_threshold, 'reset_period_ns': reset_period_ns}})
            elif(mitigation == "RRS"):
                num_hrt_entries, num_rit_entries, rss_threshold, reset_period_ns = get_rh_parameters(mitigation, tRH)
                config['MemorySystem']['Controller']['plugins'].append({'ControllerPlugin' : {'impl': 'RRS', 'num_hrt_entries': num_hrt_entries, 'num_rit_entries': num_rit_entries, 'rss_threshold': rss_threshold, 'reset_period_ns': reset_period_ns}})
            elif(mitigation == "MithrilDDR4"):
                adaptive_th, rfmTh, num_entries = get_rh_parameters(mitigation, tRH)
                config['MemorySystem']['Controller']['plugins'].append({'ControllerPlugin' : {'impl': 'MithrilDDR4', 'adaptive_threshold': adaptive_th, 'rfm_threshold': rfmTh, 'num_table_entries': num_entries}})
            elif(mitigation == "MithrilCache"):
                adaptive_th, rfmTh, num_entries = get_rh_parameters(mitigation, tRH)
                config['MemorySystem']['Controller']['plugins'].append({'ControllerPlugin' : {'impl': 'MithrilCache', 'adaptive_threshold': adaptive_th, 'rfm_threshold': rfmTh, 'num_table_entries': num_entries, 'num_cache_entries': 512, 'associativity': 512, 'write_back_en': False}})
                config['MemorySystem']['DRAM']['timing']['preset'] = "DDR4_2400R_AL16"
            elif(mitigation == "NoDefense"):
                pass

            # All simulations will be executed in parallel, so be careful to not overload your RAM
            cmd = "./ramulator -c '" + str(config) + "' > " + result_filename + " 2>&1 &"                        
            
            yaml.dump(config, config_file, default_flow_style=False)
            config_file.close()
            result_file.write(cmd + "\n")
            result_file.close()
            
            print("Running: trace = " + trace_name + ", mitigation = " + mitigation + ", tRH = " + str(tRH))
            subprocess.call(cmd, shell=True)
