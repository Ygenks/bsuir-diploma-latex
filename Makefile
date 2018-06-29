CC=latexmk
CFLAGS=-xelatex
DIPLOMA_REPORT_PDF=diploma_report
PRACTICE_REPORT_PDF=practice_report
BIBTEX=bibtex

all: diploma

diploma: *.tex
	$(CC) $(CFLAGS) $(DIPLOMA_REPORT_PDF)

practice: *.tex
	$(CC) $(CFLAGS) $(PRACTICE_REPORT_PDF)

.PHONY: clean
clean:
	rm -f *.aux *.log *.out *.toc *.gz *.gz\(busy\) *.blg *.bbl *.fls *.xdv *.fdb_latexmk
