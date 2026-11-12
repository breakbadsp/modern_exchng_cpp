"""Run perf with events disabled until bench_client enables its measured loop."""
import os
import subprocess


def run_perf(command, *, cwd):
    control_read, control_write = os.pipe()
    ack_read, ack_write = os.pipe()
    descriptors = (control_read, control_write, ack_read, ack_write)
    environment = dict(os.environ, MEX_PERF_CONTROL_FD=str(control_write), MEX_PERF_ACK_FD=str(ack_read))
    try:
        return subprocess.run(
            command[:2] + ['-D', '-1', f'--control=fd:{control_read},{ack_write}'] + command[2:],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=cwd, env=environment, pass_fds=descriptors, timeout=180)
    finally:
        for descriptor in descriptors:
            os.close(descriptor)
