all: figures bibtex pdflatex 

bibtex:
	pdflatex main.tex
	bibtex main.aux

figures:
	cd images && ls *.eps | xargs -n1 epstopdf

pdflatex:
	pdflatex main.tex
	pdflatex main.tex

clean:
	rm -f *.aux
	rm -f *.pdf
	rm -f *.log
	rm -f *.out
	rm -f *.bbl
	rm -f *.blg
	rm -f *.toc
	rm -f *.lof
