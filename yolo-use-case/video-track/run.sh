sudo ./run-isolated.sh -c 0-9 -- env PYTHONPATH="$PYTHONPATH" $(which python3) simple-track.py --output cpu_10cores

sudo ./run-isolated-stress-ng.sh -c 0-9 -l 20 -- env PYTHONPATH="$PYTHONPATH" $(which python3) simple-track.py --output cpu_10cores_stress20

