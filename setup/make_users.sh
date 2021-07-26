export MUNGEUSER=1005
groupadd -g $MUNGEUSER munge
useradd  -m -c "MUNGE Uid 'N' Gid Emporium" -d /var/lib/munge -u $MUNGEUSER -g munge  -s /sbin/nologin munge

export SlurmUSER=1001
groupadd -g $SlurmUSER slurm
useradd  -m -c "Slurm workload manager" -d /var/lib/slurm -u $SlurmUSER -g slurm  -s /bin/bash slurm

export USER1=1101
groupadd -g $USER1 user1
useradd  -m -c "test user 1" -d /home/user1 -u $USER1 -g user1 -s /bin/bash user1

export USER2=1102
groupadd -g $USER2 user2
useradd  -m -c "test user 2" -d /home/user2 -u $USER2 -g user2 -s /bin/bash user2

usermod --password $(echo test | openssl passwd -1 -stdin) user1
usermod --password $(echo test | openssl passwd -1 -stdin) user2

