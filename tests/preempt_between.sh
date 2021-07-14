# launches 2 jobs where the second will preempt the first via between QOS preemption

time_interval=${1:-1}


srun -p debug -J to_be_preempted -q deadline -c 1 --mem 1G --nodelist slurm-test-c2 sleep 1000

sleep "$time_interval"

srun -p debug -J to_preempt -c 1 -q normal --mem 1G --nodelist slurm-test-c2 sleep 1000
