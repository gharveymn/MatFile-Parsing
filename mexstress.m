userview = memory;
disp(userview.MemUsedMATLAB)

addpath('res')
addpath('bin')
%file = 'res/my_struct.mat';
%vars = {'my_struct','my_struct.array', 'my_struct.cell', 'cell', 'string'};
%file = 'C:\workspace\matlab\RonZ\data\optData_ESTrade.mat';
%vars = {'extPar'};
file = 'res/x.mat';
vars = {''};
domemory = false;
numtests = 10000;
avgtime = 0;
memvals = rand(numtests+1,1);
lents = 0;
if(domemory)
	memvals(1) = userview.MemUsedMATLAB;
	for i = 1:numtests

		getmatvar(file, vars{:});

		timestr = sprintf('%d/%d',i,numtests);
		fprintf([repmat('\b',1,lents) timestr]);
		lents = numel(timestr);
		userview = memory;
		memvals(i+1) = userview.MemUsedMATLAB;
	end
	fprintf('\n');
	plot(memvals);
	xlim([0,numtests]);
else
	for i = 1:numtests
		tic
		getmatvar(file, vars{:});
		avgtime = avgtime + toc;
		timestr = sprintf('%d/%d | avg:%5.8f',i,numtests,avgtime/i);
		fprintf([repmat('\b',1,lents) timestr]);
		lents = numel(timestr);
	end
	fprintf('\n');
end
userview = memory;
disp(userview.MemUsedMATLAB)
