include ../common/build-flags.mk

all: app/accel accel-sim/sim

app/accel: app/accel.o app/driver.o ../common/vfio-pci.o \
	../common/dma-alloc.o

accel-sim/sim: LDLIBS+=-L/simbricks/lib -lsimbricks
accel-sim/sim: accel-sim/sim.o accel-sim/plumbing.o accel-sim/dma.o
	g++ -o accel-sim/sim accel-sim/sim.o accel-sim/plumbing.o accel-sim/dma.o $(LDLIBS)

clean:
	rm -rf app/accel app/*.o accel-sim/sim accel-sim/*.o \
		out test*.out

%.out: tests/%.sim.py tests/%.check.py
	-simbricks-run --verbose --force $(SIMBRICKS_FLAGS) $<
	-python3 tests/`basename $< .sim.py`.check.py 2>&1 | tee $@

test.out: app/accel accel-sim/sim

test: test.out

check:
	-for c in tests/*.check.py; do python3 $$c; done


.PHONY: all clean check test
