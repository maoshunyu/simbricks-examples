from check_common import *

test_name('test0')

data = load_testfile(f'out/test0-1.json')

try:
  out = data['sims']['host.host']['stdout']
  line = find_line(out, '^Cycles per operation: ([0-9]*)')
  if not line:
    fail('Could not find "Cycles per operation:" output')

  cycles = int(line.group(1))

  print(f'\033[92m[RESULT HOST]\033[0m  {cycles} Cycles/op')

  sim_out = data['sims']['dev.host.accel']['stderr']
  # MMIO Write: ctrl 1 ex_time=17597496001 main=17597491001
  start = find_line(sim_out, '^MMIO Write: ctrl 1 ex_time=([0-9]*) main=([0-9]*)')
  end = find_line(sim_out, 'DONE  main=([0-9]*)')
  if not start:
    fail('Could not find CTRL=1')
  if not end:
    fail('Could not find DONE')
  start_time = int(start.group(2))
  end_time = int(end.group(1))
  sim_cycles = (end_time - start_time) / 1000 # ns
  print(f'\033[92m[RESULT SIM]\033[0m   {sim_cycles} ns/op')
except Exception:
  exception_thrown()
  fail('Parsing simulation output failed')


success()
