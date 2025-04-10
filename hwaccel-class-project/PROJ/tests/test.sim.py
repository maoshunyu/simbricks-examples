import sys; sys.path.append('./tests/')
import simbricks.orchestration.experiments as exp
import simbricks.orchestration.simulators as sim

from hwaccel_common import *

experiments = []

e = exp.Experiment(f'test0')
e.checkpoint = True

server_config = HwAccelNode()
# server_config.cores = 8

# NOT USED FOR GEM5!!
# server_config.threads = 8

server_config.app = AccelApp(8)
server_config.nockp = False

server = sim.Gem5Host(server_config)
server.name = 'host'
server.cpu_type = 'TimingSimpleCPU'
server.cpu_freq = '1GHz'
#server.variant = 'opt'
# server.extra_main_args = ['--debug-flags=SimBricksPci,DMA']

hwaccel = HWAccelSim()
hwaccel.name = 'accel'
hwaccel.sync = True

server.pci_latency = 100
server.sync_period = 100

hwaccel.pci_latency = 100
hwaccel.sync_interval = 100

server.add_pcidev(hwaccel)

e.add_pcidev(hwaccel)
e.add_host(server)
server.wait = True

experiments.append(e)
