# launches 2 jobs where the second will preempt the first via WITHIN

time_interval=${1:-1}

srun -p debug -J -q normal to_be_preempted -c 1 --mem 1G --nodelist slurm-test-c2 --priority=100 sleep 1000

sleep "$time_interval"

srun -p debug -q normal -J to_preempt -c 1 --mem 1G --nodelist slurm-test-c2 --priority=101 sleep 1000
