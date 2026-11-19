#!/bin/sh
# m4demo.sh - Milestone 4 in one script: processes, background jobs, pipes, PATH
echo "== 1. a separate program with arguments (crt0: argc / argv / envp)"
hello one two

echo "== 2. two background programs run while this script continues"
counter A 250 4 &
counter B 400 3 &
ps

echo "== 3. a pipeline of separate programs connected by kernel pipes"
ls /bin | grep -c .
cat /docs/planets.txt | grep ice | wc -l

echo "== 4. where commands come from (PATH)"
type ls cd hello
echo "PATH=$PATH"

echo "== 5. wait for the background jobs, then the scheduler statistics"
wait
sched
echo "== done"
