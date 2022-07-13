# ROACH2 Control Scripts
To use the GUI software, you need python2 and pyqt installed.
* install python2
1. I suggest you intall conda first, and the create a python2 env:
```
	conda create -n fast_roach2_py2 python==2.7
	conda activate fast_roach2_py2
```
If you can't create python2.7 env, try the following commands:
```
	conda create -c free -n fast_roach2_py2 python==2.7.9
```
2. install pyqt
```
	conda install pyqt
```
3. You need to install some necessary packages:
```
	pip install katcp==0.5.5
	pip install termcolors
	pip install numpy
	pip install pyqtgraph
```


