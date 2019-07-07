/**** Includes  ****/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>							//allows functions to accept an indefinite number of arguments	
#include <string.h>
#include <ctype.h>							// playing with characters
#include <unistd.h>
#include <termios.h>	 						// library for terminal stuff
#include <errno.h>							// for error handling
#include <fcntl.h>							//file control options
#include <sys/ioctl.h>							//Input Output Control
#include <sys/types.h>		
#include <time.h>											


/**** Defines ****/ 

#define MINOCH_VERSION "0.0.1"						//version 
#define MINOCH_TAB_STOP 8				
#define MINOCH_QUIT_TIMES 2						// nb of times pressing ctrl-q to exit

#define CTRL_KEY(k) ((k) & 0x1f)					
// 0x1f = 00011111  : why we use and 0x1f becaus ctrl+key in terminal does the same, it takes binary of the key makes bit 5,6,7 to zero and sends the resulting byte  


enum editorKey{
	
  BACKSPACE = 127,
  ARROW_LEFT = 10000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  PAGE_UP,
  PAGE_DOWN
};



/**** Data ****/ 

typedef struct erow {					//editor row structure that will store our  txt lines
  int size;
  int rsize;
  char *chars;
  char *render;						// for handling tabs 
} erow;




struct editorConfig{
	int cx, cy;								// cursor x, y positions
	int rx;									// index for the render field (the tabs field)	
	int rowoff;								//row offset for vertical scrolling	
	int coloff;								// columns offset for horizontal scrolling
	int screenrows;									
	int screencols;
	int numrows;								//number of rows to be written
 	erow *row;								// editor row : a struct that holds text row ( the characters and the
	int dirty;								// variable to warn us if file's been changed or not	
	char *filename;
	char statusmsg[80];							//status msg (we'll use it for searching in the file) 
	time_t statusmsg_time;							//we will erase the message after few seconds 

	struct termios original_termios;					// save original terminal attributes..
};


struct editorConfig E;



/// Prototype : so we can use functions defined later in the file, wherever we want !

void editorSetStatusMessage(const char *fmt, ...);




/**** Terminal ****/

void die(const char *s){							// for error handling; C programms set the global  variable
	write(STDOUT_FILENO, "\x1b[2J", 4);	
	write(STDOUT_FILENO, "\x1b[H", 3);
	
	perror(s);								// errno to indicate the error; perror looks at the global errno and prints a discriptive error msg
	exit(1);
	}



void disableRawMode(){										//function that disables raw mode; we will use it right before leaving the editor.
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios) == -1 )			// set attributes of terminal back to original ones .. if we fail we throw error and die 
	   die("tcsetattr");	
}	




// we get terminal attributes, modify them, then pass them back
void enableRawMode(){  												//function that will enable raw mode ! 
	
	if(tcgetattr(STDIN_FILENO, &E.original_termios) == -1) die ("tcgetattr");
	atexit(disableRawMode);
	
	struct termios raw = E.original_termios;
	
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP| IXON); 		 //iflag stands for input flags : (IXON;turn off ctrl-s that pauses transmission and ctrl-q that resumes it back..)	
														//(ICRNL; fix ctrl-m CR carriage return NL new line)
	raw.c_oflag &=  ~(OPOST);										// output flag, (OPOST;)
	raw.c_cflag |=(CS8);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);  							//local flags : (ECHO;printing to the screen) (ICANON;turn off canonical mode)
							// (IEXTEN; disable ctrl-v that will print next caractere literrarly (no escaping)) (ISIG;turn off ctrl-c and ctrl-z)
  	
  	raw.c_cc[VMIN] = 0;											//minimum number of input bytes needed before read() can return 
        raw.c_cc[VTIME] = 1;           										// max time to wait before read() can return 100ms 
  	
  	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1 ) die("tcsetattr");
	
	}


int editorReadKey(){										// function to read the keypresses
	int nread;
	char c;
	while((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if(nread == -1 && errno != EAGAIN) die ("read");
	}
	
	if(c == '\x1b'){ 							// when the user enters an escape, we immediately read 2 more bytes into seq buffer, if reads time out we assume user just pressed escape key and return that. otherwise we look which arrow key sequence was and return it.
		char seq[3];
		if(read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if(read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

	    if(seq[0] == '['){
			if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
          }
        }
      } else {			
			switch (seq[1]) {
				case 'A': return ARROW_UP;
				case 'B': return ARROW_DOWN;
				case 'C': return ARROW_RIGHT;
				case 'D': return ARROW_LEFT;
			
			}
		}
	}
		return '\x1b';
	}else { 
		return c;
	}
}






int getWindowSize(int *rows, int *cols){
	struct winsize ws;
	
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col ==0){
	  return -1;
	}else{
	  *cols = ws.ws_col;
	  *rows = ws.ws_row;
	  return 0;
	}

}

/**** Row operations ****/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (MINOCH_TAB_STOP - 1) - (rx % MINOCH_TAB_STOP);
    rx++;
  }
  return rx;
}


void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;
  free(row->render);
  row->render = malloc(row->size + tabs*(MINOCH_TAB_STOP - 1) + 1);
  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % MINOCH_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}



void editorInsertRow(int at, char *s, size_t len) {

  if (at < 0 || at > E.numrows) return;				//validate the index 

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));	//allocate memo for one more erow
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}


void editorFreeRow(erow *row) {
  free(row->chars);
}


void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}





void editorRowInsertChar(erow *row, int at, int c){			//"at" is the index we will insert the char at,
	if(at < 0 || at > row->size) at = row->size;			
	row->chars = realloc(row->chars, row->size +2);			// zow->size+2; the +2 : 1 byte for the char we will insert the second foe the null byte
	memmove(&row->chars[at+1], &row->chars[at], row->size-at +1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	E.dirty++;

}

void editorRowAppendString(erow *row, char *s, size_t len) {	// this function s gonna be used when we delete something from begining of a line, so the content of that line is going up to line above !
  row->chars = realloc(row->chars, row->size + len + 1);		//  we allocate memo for row->size + len + 1 for the null byte
  memcpy(&row->chars[row->size], s, len);			// we then move the content to the end of above line 
  row->size += len;						// update new size
  row->chars[row->size] = '\0';					// add null byte
  editorUpdateRow(row);						// update the row 
  E.dirty++;							// dirty bit updated ! 
}




void editorRowDelChar(erow *row, int at){				//function to delete a character argument at is the index !
	if(at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at+1], row->size -at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}




/**** Editor Operations ****/


void editorInsertChar(int c){
	if(E.cy == E.numrows){				// if we'r at the end of the file 
		editorInsertRow(E.numrows, "", 0);		// then we append a new row before inserting in it
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}


void editorInsertNewline(){

	if(E.cx == 0 ){						// if we'r at the begining of a line,  insert a blank row 
		editorInsertRow(E.cy, "", 0);
	}else{
	  erow *row = &E.row[E.cy];
	  editorInsertRow(E.cy +1, &row->chars[E.cx], row->size - E.cx);
	  row = &E.row[E.cy];
	  row->size = E.cx;
	  row->chars[row->size] = '\0';
	  editorUpdateRow(row);
	}
	E.cy++;
	E.cx =0;
}





void editorDelChar(){

	if(E.cy == E.numrows)	return;					// if the cursor went after the end of the file, we just return  
	if (E.cx == 0 && E.cy == 0) return;				//


	erow *row = &E.row[E.cy];					// we get the errow where the cursor is ..
	if(E.cx > 0){							//if we'r not at the begining of the line 
		editorRowDelChar(row, E.cx-1);				// delete char and
		E.cx--;							//decrement cursor on x axis ! 
	} else {
    	E.cx = E.row[E.cy - 1].size;
    	editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    	editorDelRow(E.cy);
    	E.cy--;
  	}
}







/*** File i/o ***/

char *editorRowsToString(int *buflen){		// this function transforms all the rows in one string to store it in a file on disk 
	int totlen =0;
	int j;
	for(j=0; j< E.numrows; j++)			// we add up all the lines sizes (the +1 is for the '\n' at the end of each line)
		totlen += E.row[j].size +1;
	*buflen = totlen;				// we store the lenght on buflen 

	char *buf = malloc(totlen);			// we allocate memory
	char *p = buf;
	for(j=0; j < E.numrows; j++){			// and loop over the lines
		memcpy(p, E.row[j].chars, E.row[j].size);		//copy the content of the lines in the 'p' buffer
		p += E.row[j].size;
		*p = '\n';							// add the '\n'
		p++;	
	}

	return buf;
}




void editorOpen(char *filename) {			// function to open files, we read line by line from the file we want to open !

free(E.filename);
E.filename = strdup(filename);  				//get filename and store it ! strdup from string.h makes a copy of its argument

FILE *fp = fopen(filename, "r");
  if(!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
	                   line[linelen - 1] == '\r'))
      linelen--;
    
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}



void editorSave(){
	if(E.filename == NULL) return;

	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);       	// O_RDWR : open for read/write.  O_CREAT : to create the file if it doesnt exist ! 0644 the permissions !
	if (fd != -1) {
    		if (ftruncate(fd, len) != -1) {
      			if (write(fd, buf, len) == len) {
        			close(fd);
        			free(buf);
				E.dirty = 0;
				editorSetStatusMessage("Saving ...");
        			return;
      			}
    		}
    		close(fd);
  	}
	free(buf);
	editorSetStatusMessage("Error while saving : %s",  strerror(errno));				//strerror from string.h takes errno global as argument and prints human readable message error
}




/**** Append buffer ****/

struct abuf {							//the buffer that will temporarly store what we want to write in the screen of our editor (like welcome msg for ex..)
	char *b;
	int len;
		
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len){
	char *new = realloc(ab->b, ab->len + len);
	
	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
	
}

void abFree(struct abuf *ab){
	free(ab->b);
	}



/**** Output ****/

void editorScroll(){	
 	E.rx = 0;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}

//Vertical scrolling 	
// this function will help us set our row offset, so that whenever the cursor is no more visible we adjust the rowoffset variable so that it s inside the visible window (in other words we scroll down )
	if(E.cy < E.rowoff){			// checking if the cursor is above visible window ; if that s the case we set the row offset to the cursor position
		E.rowoff = E.cy;
	}
	if(E.cy >= E.rowoff + E.screenrows){			// checking if the cursor is down the visible window; if that s the case then 
		E.rowoff = E.cy - E.screenrows +1;
	}
//Horizontal scrolling, same logic as the vertical scrolling; we look for the cursor position and compare it to the offset !
	if (E.rx < E.coloff) {					// if the cursor is in the left of the offset 
   		E.coloff = E.rx;
  	}

	if(E.rx > E.coloff + E.screencols){
		E.coloff = E.rx - E.screencols +1;
	}
}




void editorDrawRows(struct abuf *ab){							//function that will draw the contour of our editor, print the welcome message and print the text (row by row) 			
	int y;
	for(y=0; y < E.screenrows; y++){
		int filerow = y + E.rowoff;			// variable for row + offset (used for scrolling)
		if (filerow >= E.numrows) {
			if(E.numrows == 0 && y == E.screenrows / 3){
				char welcome[100];
				int welcomelen = snprintf(welcome, sizeof(welcome), "Minoch text editor -- version %s : Mainstream Algerian cat name  ", MINOCH_VERSION);
				
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "*", 1);
					padding --;
				}
				while (padding--) abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcomelen);
			} 
			else {
				abAppend(ab, "*", 1);
			}
		}
		else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len  = 0;
			if (len > E.screencols) len = E.screencols;
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
						
		abAppend(ab, "\x1b[K", 3);						
		abAppend(ab, "\r\n", 2);						//print new line at the end	
		}	
}



void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);	 											// this escape sequence will invert the colors black txt on white background 
  
  char status[100],nblinestatus[100];
  int len = snprintf(status, sizeof(status), "%.20s : %d lines %s", E.filename ? E.filename : "Untitled Document", E.numrows, E.dirty ? "(modified)" : "");  	//preparing the filename & nb of lines
  int nblinelen = snprintf(nblinestatus, sizeof(nblinestatus), "Current line :%d", E.cy +1); 				//preparing the nb of each line stored in E.cy; we add +1 becaus E.cy starts at 0
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);												//printing the filename & nb of lines
  while (len < E.screencols) {
    if(E.screencols - len == nblinelen){
	  abAppend(ab, nblinestatus, nblinelen);
	  break;
    }else{
    	  abAppend(ab, " ", 1);
	  len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);											// escape sequence that will make colors back to normal
  abAppend(ab, "\r\n", 2);
}




void editorDrawMessageBar(struct abuf *ab) {																		
	abAppend(ab, "\x1b[K", 3);										//escape sequence that Clears line from cursor right  !
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;							// we make sure the msg fits in the screen (comparing lenght with E.screencols)
	if (msglen && time(NULL) - E.statusmsg_time < 5)							// we print the msg if it s 5 secondes old !
	  abAppend(ab, E.statusmsg, msglen);
}




void editorRefreshScreen(){		// we make a buffer that stores all what we want to write to the terminal, and then write to it at the end 												// function to clear the screen 	
	editorScroll();	
	struct abuf ab = ABUF_INIT;
	
	abAppend(&ab, "\x1b[?25l", 6); // to hide the cursor
	
//	abAppend(&ab, "\x1b[2J", 4);	we comment this line becaus we want to clear each line alone, check editorDrwRows
/* 4 means we'r writing 4 bytes to terminal, first one is the hex \x1b = 27 in decimal  which is the escape character
 \x1b[2J is an escape sequence(responsible for text formatting like coloring text, clearning the screen) : they all start with 27[ : we are using J command that takes 2 as argument. that will clear the entire screen (<esc>[0J will clear the screen from cursor up to the end, <esc>[1J will clear screen up to where cursor is)
*/

	abAppend(&ab, "\x1b[H", 3);
/* escape sequence with H command that reposition the cursor on the screen, takes  2 args row nb, and col nb :default arg are 1 so, if
 \x1b[H is the same as \x1b[1;1H and that will position the cursor on first row, first column (rows and cols are numbered starting from 1 not 0 )

*/

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);
	
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);				// we add +1 becaus terminal indexing starts from 1 and not 0
	abAppend(&ab, buf, strlen(buf));
	
	

	abAppend(&ab, "\x1b[?25h", 6);	// to show cursor back again

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);

}

void editorSetStatusMessage(const char *fmt, ...) {						// this is a "variadic"(not sure how to spell it :p) function that have an undefined nb of args
	va_list ap;										// declare a variadic list of functions  : ap	
	va_start(ap, fmt);									// give last known argument
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);					
	va_end(ap);
	E.statusmsg_time = time(NULL);								// current timestamp (nb of seconds from 1st jan 1970)
}



/**** Input ****/

void editorMoveCursor(int key){					// function that maps arrow keys to moving x,y positions of cursor
	
 	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	switch(key){
		case ARROW_LEFT:
		  if(E.cx != 0){
		    E.cx--;
		  }else if (E.cy > 0){				// if E.cx is Null and we'r not at the first line;(begining of a line and we press left)
		    E.cy --;					// then move up one line 
		    E.cx = E.row[E.cy].size;			// and put cursor at end of that line(the end of the line  = the size of text there !) 
		  }
		  break;
		case ARROW_RIGHT:
		  if (row && E.cx < row->size) {			// if there is a txt row and the cursor is not at the end  		
		    E.cx++;						// then we can move to the right
		  }else if (row && E.cx == row->size){			// however, when we get to the end of that txt line
		    E.cy++;						// we need to go to next line 
		    E.cx = 0;						// and put the cursor at the begining of that "next" line
		  }
		  break;
		case ARROW_UP:
		  if(E.cy != 0){
		    E.cy--;
		  }
		  break;
		case ARROW_DOWN:
		  if(E.cy < E.numrows){
		    E.cy++;
		  }
		  break;	
	}

	  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];			// we need to handle the situation where the cursor goes beyond the end of a line
  	  int rowlen = row ? row->size : 0;					// if row is null, its size is 0, else its size is the row->size
 	  if (E.cx > rowlen) {							// if the cursor goes beyond the end of the line to  the right (E.cx > rowlen) we bring it back to the end of the line 
   	  	E.cx = rowlen;
	  }
		
}




void editorProcessKeypress(){												// function that maps the keypresses to our functionnalities, eg(ctrl+q = quit)
	static int quit_times = MINOCH_QUIT_TIMES;			// number of times  u have to press ctrl-Q to quit when  u have unsaved changes !

	int c = editorReadKey();	
	switch(c){

		case '\r':
		  editorInsertNewline();
		  break;


		case CTRL_KEY('q'):
		  if(E.dirty && quit_times> 0){
			editorSetStatusMessage("WARNING ! Unsaved changes. Press Ctrl-Q %d more times to quit", quit_times);
			quit_times--;
			return;
		  }

		  write(STDOUT_FILENO, "\x1b[2J",4);
		  write(STDOUT_FILENO, "\x1b[H", 3);
		  exit(0);
		  break;


		case CTRL_KEY('s'):
		  editorSave();
		  break;	
	
		case BACKSPACE:
		case CTRL_KEY('h'):	
		  editorDelChar();
		  break;


		case PAGE_UP:
		case PAGE_DOWN:
		  {
			int times = E.screenrows;
			while (times--)
			  editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
		  }
		  break;  
		 
		  
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
		  editorMoveCursor(c);
		  break;
	
		case CTRL_KEY('l'):
		case '\x1b':
		  break;
		
		default:
		  editorInsertChar(c);
		  break;
	}
	quit_times = MINOCH_QUIT_TIMES;					//  reset the quit_times, when the ctrl-q are not "continuous" 
}


/**** Initializing ****/

void initEditor(){
E.cx = 0;																// initialize cursor position
E.cy = 0;
E.rx = 0;
E.rowoff = 0;
E.coloff = 0;
E.numrows = 0;
E.row = NULL;
E.dirty = 0;
E.filename = NULL;
E.statusmsg[0] = '\0';
E.statusmsg_time = 0;

if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");

E.screenrows -= 2;

}


// 666 Hail satan 666 :D !! 

/**** Main function****/ 

int main(int argc, char *argv[]){
	enableRawMode();
	initEditor();

	if(argc >= 2){
	  	editorOpen(argv[1]);
	}

	editorSetStatusMessage("*** HELP: Ctrl-S = Save | Ctrl-Q = Exit");


	while(1){
	    editorRefreshScreen();
   	    editorProcessKeypress();
	
	}
return 0;	
}
