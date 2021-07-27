file_or_folder=$1
shift
hosts=$*

echo Copying ${file_or_folder}
echo to ${hosts}

for host in "$@" 
do
    cmd="ssh $host  mkdir -p `dirname ${file_or_folder}` && rsync -avzP --rsh=ssh ${file_or_folder} $host:${file_or_folder}"
    echo $cmd
    eval $cmd
done
