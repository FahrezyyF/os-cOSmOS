# fsdemo.sh - recursive copy / move / remove of folders (bonus commands)
mkdir -p /playground/a/b
echo "deep file" > /playground/a/b/deep.txt
cp -r /playground /playground-copy
mv /playground-copy /playground/copy
tree /playground
rm -r /playground
echo "playground removed"
