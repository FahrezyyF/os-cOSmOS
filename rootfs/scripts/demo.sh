# demo.sh - shell features in one script: variables, pipes, redirection, && and ||
echo "== variables"
NAME=cOSmOS
echo "running on $NAME, last status $?"

echo "== pipes: gas and ice giants, reverse sorted"
cat /docs/planets.txt | grep -E "gas|ice" | sort -r

echo "== redirection: write, append, read back"
echo "line one" > /tmp-demo.txt
echo "line two" >> /tmp-demo.txt
cat /tmp-demo.txt | wc
rm /tmp-demo.txt

echo "== conditionals"
find planets.txt > /dev-null.txt && echo "found planets.txt"
rm /dev-null.txt
ls /nowhere || echo "ls failed as expected, status $?"
echo "== done"
