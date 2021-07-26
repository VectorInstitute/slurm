apt-get update
apt-get install git gcc make ruby ruby-dev libpam0g-dev libmariadb-client-lgpl-dev libmysqlclient-dev -y

# MUNGE
apt-get install libmunge-dev libmunge2 munge -y 
systemctl enable munge
systemctl start munge

# DB and sql
apt-get install mariadb-server -y
systemctl enable mysql
systemctl start mysql

echo Testing Munge Installation
munge -n | unmunge | grep STATUS

mysql -u root  < create_slurm_db.sql 
