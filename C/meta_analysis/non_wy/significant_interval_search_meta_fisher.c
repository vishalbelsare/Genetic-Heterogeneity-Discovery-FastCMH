#ifndef _significant_interval_search_meta_fisher_c_
#define _significant_interval_search_meta_fisher_c_

/* LIBRARY INCLUDES */
#include<math.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>

/* CODE DEPENDENCIES */
#include"time_keeping.c"
#include "../../EasyGWAS/chi2.h"

/* MACROS */

/* CONSTANT DEFINES */
#define READ_BUF_SIZ 524288 //Size of buffer used to read files
#define NGRID 500 //Number of samples in the grid of tentative corrected significance thresholds, not counting 1 as it is a trivial threshold
#define LOG10_MIN_PVAL -30.0 //Minimum tentative corrected significance threshold = 10^{LOG10_MIN_PVAL}

/* -------------------------------------------- GLOBAL VARIABLES -----------------------------------------------------------*/

FILE *pval_out_file; // File to output the P-values of testable intervals
FILE *sig_int_file; // File to output the P-values of significant intervals
FILE *timing_file; // File to output information about the runtime of the algorithm
FILE *summary_file; // File to output varied information about the process (number of interval processed and so on)
FILE *maxfish_hist_file; // File to output histogram of maximum attainable Fisher's combined statistics (only reliable in the testable range)

// Number of observations, N
long long N;
// Number of observations in positive class
long long n;
// Sequence length
long long L;
long long L_max;
// Number of tables
long long K;
// Number of observations per table
long long *Nt;
// Number of observations in positive class per table
long long *nt;
// Number of observations per table
long long *cum_Nt; //Cumulative sum of Nt
// Now some precomputed quantities to save time when computing Fisher's combined test statistic
long long *hypercorner_bnd; // max(ni,Ni-ni) for each of the K tables
double *gammat; // ni/Ni for each of the K tables
double *gammabint; // (ni/Ni)*(1-ni/Ni) for each of the K tables
// Cell counts per table
long long *at;

// Current interval length
long long l;
// Number of testable intervals
long long m;
// Target FWER
double alpha;
// Final corrected significance threshold
double delta_opt;

// Grid of logarithmically spaced corrected significance thresholds. The sequence is ordered
// from larger thresholds to smaller thresholds.
double *pgrid;
// Current tentative corrected significance threshold and index of it in the grid
double pth;
int idx_th;
// Step size in the grid
double log10_p_step;

// Array with all values of log(n!) in [0,N] pre-computed
double *loggamma;
// K x max(Nt[k]+1,k=0,...,K-1) matrix such that the k-th row has all values of minimum attainable log P-value in [0,Nt[k]] pre-computed
double **logpsi;

// Vector of class labels
char *Y_tr;
// The original dataset matrix stored as a LxN matrix, where L is the sequence length
// and N the number of observed sequences. This exploits the row-major storage of C matrices.
char **X_tr;
// Another LxN matrix, storing the values of nodes in the layer above.
char **X_par;

// A LxK-dimensional matrix storing the frequency of each interval in each table as they are processed
long long **freq_par;
// A (Ngrid+1)-dimensional vector such that freq_cnt[j] = #intervals with maximum attainable
// Fisher's combined statistic in the j-th bucket
long long *freq_cnt;
// A L-dimensional vector storing the minimum attainable p-values of each interval as
// they are processed
double *pmh_min_par;

// Queue of testable intervals in the layer below
long long *testable_queue;
long long testable_queue_front;
long long testable_queue_length;

// Auxiliary variable to keep track of current layer
long long last_tau;

/* PROFILING VARIABLES */
long long n_intervals_processed;
long long n_pvalues_computed;
long long n_significant_intervals;

/* FUNCTION DECLARATIONS */
void loggamma_init();
void logpsi_init(double *, long long, long long);
void get_N_n(char *);
void read_labels_file(char *);
void get_L(char *);
void read_dataset_file(char *, char *);
void get_K(char *);
void read_covariates_file(char *);
inline int bucket_idx(double);
// A extern function related to the Chi2 distribution
extern double Chi2_sf(double,double); //Chi2 Survival function, from Dominik's EasyGWASCore

/* -------------------------------- INITIALISATION AND TERMINATION FUNCTIONS ----------------------------------------- */

/* Initialise the main variables, call I/O routines and allocate memory
 * Input arguments are self-explanatory
 * */
void sis_init(char *X_filename, char *Y_filename, char *C_filename, double target_fwer, long long l_max){
	long long j, Nt_max; //Loop variable
	double tic,toc;//Internal ones, do not overwrite the ones in time_keeping.c
	double log10_p;

	// Compute total number of observations and number of observations in minority class
	tic = measureTime();
	get_N_n(Y_filename);
	get_L(X_filename);
	get_K(C_filename);
	toc = measureTime();
	time_IO += toc-tic;

	// Store core constants
	alpha = target_fwer;
	L_max = l_max;

	// Initialise grid of candidate corrected significance thresholds
	pgrid = (double *)malloc((NGRID+1)*sizeof(double));
	for(log10_p=0,log10_p_step=-LOG10_MIN_PVAL/NGRID,j=0; j<=NGRID; log10_p-=log10_p_step, j++) pgrid[j] = pow(10,log10_p);
	// Initialise threshold values
	idx_th = 1; pth = pgrid[idx_th];

	// Initialise cache for log(x!)
	loggamma_init();

	// Allocate space for per table number of observations and number of observations in positive class
	Nt = (long long *)calloc(K, sizeof(long long));
	if(!Nt){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array Nt\n");
		exit(1);
	}
	nt = (long long *)calloc(K, sizeof(long long));
	if(!nt){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array nt\n");
		exit(1);
	}
	cum_Nt = (long long *)calloc(K+1, sizeof(long long));
	if(!cum_Nt){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array cum_Nt\n");
		exit(1);
	}
	at = (long long *)calloc(K, sizeof(long long));
	if(!at){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array at\n");
		exit(1);
	}

	// And read covariates file, filling in the array Nt
	tic = measureTime();
	read_covariates_file(C_filename);
	toc = measureTime();
	time_IO += toc-tic;

	// Allocate space for class labels
	Y_tr = (char *)malloc(N*sizeof(char));
	if(!Y_tr){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array Y_tr\n");
		exit(1);
	}
	// And store them in memory from file, also computing nt along the way
	tic = measureTime();
	read_labels_file(Y_filename);
	toc = measureTime();
	time_IO += toc-tic;

	// Initialise dataset matrix

	X_tr = (char **)malloc(L*sizeof(char *));
	if(!X_tr){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array X_tr\n");
		exit(1);
	}

	X_tr[0] = (char *)calloc(L*N, sizeof(char));
	if(!X_tr[0]){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array X_tr[0]\n");
		exit(1);
	}
	for(j=1; j<L; j++) X_tr[j] = X_tr[0] + j*N;
	// Same for parents
	X_par = (char **)malloc(L*sizeof(char *));
	if(!X_par){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array X_par\n");
		exit(1);
	}
	X_par[0] = (char *)calloc(L*N, sizeof(char));
	if(!X_par[0]){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array X_par[0]\n");
		exit(1);
	}
	for(j=1; j<L; j++) X_par[j] = X_par[0] + j*N;

	tic = measureTime();
	read_dataset_file(X_filename, X_tr[0]);
	toc = measureTime();
	time_IO += toc-tic;


	// Allocate memory for several vectors

	// First some small vectors to precompute some common magnitudes used to evaluate the test statistics
	hypercorner_bnd = (long long *)calloc(K, sizeof(long long));
	if(!hypercorner_bnd){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array hypercorner_bnd\n");
		exit(1);
	}
	gammat = (double *)calloc(K, sizeof(double));
	if(!gammat){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array gammat\n");
		exit(1);
	}
	gammabint = (double *)calloc(K, sizeof(double));
	if(!gammabint){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array gammabint\n");
		exit(1);
	}
	// Fill them in
	for(j=0; j<K; j++){
		hypercorner_bnd[j] = (nt[j] > (Nt[j]-nt[j])) ? nt[j] : Nt[j]-nt[j];
		gammat[j] = ((double)nt[j])/Nt[j];
		gammabint[j] = gammat[j]*(1-gammat[j]);
	}

	// Allocate memory for psi, raising error if it fails
	Nt_max = 0;
	for(j=0; j<K; j++) {
		if(Nt[j]>Nt_max) Nt_max = Nt[j];
	}
	logpsi = (double **)malloc(K*sizeof(double *));
	if(!logpsi){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array logpsi\n");
		exit(1);
	}
	logpsi[0] = (double *)malloc(K*(Nt_max+1)*sizeof(double));
	if(!logpsi[0]){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array logpsi[0]\n");
		exit(1);
	}
	for(j=1; j<K; j++) logpsi[j] = logpsi[0] + j*(Nt_max+1);
	// Initialise cache for log(psi(x))
	for(j=0; j<K; j++) logpsi_init(logpsi[j],nt[j],Nt[j]);


	// Now some larger data structures used during the enumeration procedure
	testable_queue = (long long *)calloc(L, sizeof(long long));
	if(!testable_queue){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array testable_queue\n");
		exit(1);
	}
	freq_par = (long long **)calloc(L, sizeof(long long *));
	if(!freq_par){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array freq_par\n");
		exit(1);
	}
	freq_par[0] = (long long *)calloc(L*K, sizeof(long long));
	if(!freq_par[0]){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array freq_par[0]\n");
		exit(1);
	}
	for(j=1; j<L; j++) freq_par[j] = freq_par[0] + j*K;
	freq_cnt = (long long *)calloc((NGRID+1), sizeof(long long));
	if(!freq_cnt){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array freq_cnt\n");
		exit(1);
	}
	pmh_min_par = (double *)calloc(L,sizeof(double));
	if(!pmh_min_par){
		fprintf(stderr,"Error in function sis_init: couldn't allocate memory for array pmh_min_par\n");
		exit(1);
	}
}

/* Precompute values of log(x!) storing them in the array loggamma */
void loggamma_init(){
	long long x;
	// Allocate memory for log-gamma cache, raising error if it fails
	loggamma = (double *)malloc((N+1)*sizeof(double));
	if(!loggamma){
		fprintf(stderr,"Error in function loggamma_init: couldn't allocate memory for array loggamma\n");
		exit(1);
	}
	// Initialise cache with appropriate values
	for(x=0;x<=N;x++) loggamma[x] = lgamma(x+1);//Gamma(x) = (x-1)!
}

/* Precompute minimum attainable P-values $\psi(x)$ for all x in [0,N] and store them the row of logpsi pointed by logpsi_row
* NOTE: IN THIS FUNCTION, WE ARE MASKING THE GLOBAL VARIABLES n and N SINCE WE WILL GET P-VALUES FOR EACH OF THE K TABLES!!!
* */
void logpsi_init(double *logpsi_row, long long n, long long N){
	double xi1;
	long long x, x_init;
	long long N_over_2;
	FILE *debug_file;

	/* Fix computation of minimum attainable P-values when n > N-n */
	n = (n > (N-n)) ? (N-n) : n;

	/* Initialise caches with appropriate values */
	N_over_2 = (N % 2) ? (N-1)/2 : N/2;//floor(N/2)

	// First compute the left side of "the W", i.e. the range [0,n]
	logpsi_row[0] = 0;
	//In this range, the recursion $\psi(x)$=$\psi(x-1)$*[(n-(x-1))/(N-(x-1))] can be seen to be correct
	for(x=1; x<=n; x++) logpsi_row[x] = log(((double)(n-(x-1)))/(N-(x-1))) + logpsi_row[x-1];

	// Now, start computing xi1 in the range [N-N_over_2,N] using another recursion, this time
	// starting in N
	// Note that we don't need to store all values, since this will be used only to initialise
	// psi[N_over_2]
	x_init = N-N_over_2;
	xi1 = 0;
	//In this range, the recursion $\xi_{1}(x)$=$\xi_{1}(x+1)$*[((x-1)-n)/(x+1)] can be seen to be correct
	for(x=(N-1); x>=x_init; x--) xi1 = log(((double)((x+1)-n))/(x+1)) + xi1;

	// Now, use the value of $\xi_{1}(N-N_over_2)$=xi1[0] to get $\psi(N_over_2)$=psi[N_over_2] using the
	// same recursion if N is odd, or simply copy the value of xi1[0] since $\xi_{1}(N-N_over_2)=\psi(N_over_2)$
	// if N is even
	if (N % 2) logpsi_row[N_over_2] = log(((double)(x_init-n))/x_init) + xi1;
	else logpsi_row[N_over_2] = xi1;

	// Now, using psi[N_over_2] compute the right side of "the W", i.e. the range [n+1,N_over_2]
	// using the same recursion as for $\xi_{1}$
	for(x=(N_over_2-1); x > n; x--) logpsi_row[x] = log(((double)((x+1)-n))/(x+1)) + logpsi_row[x+1];

	// Finally, since $\psi(x)$ is symmetric around N_over_2, complete the right half by symmetry
	for(x=x_init; x<=N; x++) logpsi_row[x] = logpsi_row[N-x];

	// Finally, take the logarithm
	//for(x=0; x<=N; x++) logpsi_row[x] = log(logpsi_row[x]);

	// Correct minimum attainable P-value in some edge-cases
	if((N % 2)==0){
		if (n == (N/2)) for(x=1; x<N; x++) logpsi_row[x] += log(2);
		else logpsi_row[N/2] += log(2);
	}

	debug_file = fopen("debug.txt", "a");
	for(x=0; x<N; x++) fprintf(debug_file, "%e\t", logpsi_row[x]);
	fprintf(debug_file, "%e\n", logpsi_row[N]);
	fclose(debug_file);
}

void output_maxfish_histogram(){
    long long i;
    for(i=0; i<=NGRID; i++) fprintf(maxfish_hist_file, "%lld\t%lld\n", i, freq_cnt[i]);   
}

/* Free all allocated memory and give some output for debugging purposes */
void sis_end(){
	// Execution time and peak memory consumption
	profileCode();

    // Output histogram of max attainable Fisher's combined statistics (only reliable in the testable range)
    output_maxfish_histogram();    

	// Free allocated memory
	free(loggamma); free(logpsi[0]); free(logpsi);
	free(Nt); free(nt); free(cum_Nt); free(at);
	free(hypercorner_bnd); free(gammat); free(gammabint);
	free(pgrid);
	free(Y_tr);
	free(X_tr[0]); free(X_par[0]);
	free(X_tr); free(X_par);
	free(freq_par[0]); free(freq_par); free(freq_cnt);
	free(testable_queue);
	free(pmh_min_par);

	// Close files
	if(pval_out_file) fclose(pval_out_file);
	fclose(sig_int_file); fclose(timing_file); fclose(summary_file); fclose(maxfish_hist_file);
}

/* ---------------------------------------FUNCTIONS TO FIND THE SIGNIFICANT INTERVALS-------------------------------- */

/* Evaluate Fisher's exact test on a table with margins x, n and N and cell count a. Note that n and N are defined as global variables.
 * The p-value is defined as a two-tailed p-value which adds up the probabilities of all tables less or equally likely to occur than the
 * one we observed
 * NOTE: IN THIS FUNCTION, WE ARE MASKING THE GLOBAL VARIABLES n and N SINCE WE WILL GET P-VALUES FOR EACH OF THE K TABLES!!!
 */
double fisher_pval(long long a, long long x, long long n, long long N){
	long long a_min, a_max, k;
	double p_left, p_right, pval;
	double pre_comp_xterms;

	// Compute the contribution of all terms depending on x but not on a
	pre_comp_xterms = loggamma[n] + loggamma[N-n] - loggamma[N] + loggamma[x] + loggamma[N-x];
	a_min = ((n+x-N) > 0) ? (n+x-N) : 0;//max(0,n+x-N)
	a_max = (x > n) ? n : x;//min(x,n)

	// The algorithm to compute the p-value proceeds as follows. We inspect simultaneously probability values on the left and right tails of the
	// hypergeometric distribution, "accepting" each time the one that is smaller. When that happens, we move the index in the appropriate direction,
	// that is, a_min++ if we "accept" a value on the left and a_max-- if we "accept" a value on the right. As soon as the "accepted" value is located
	// in index a, we know that we have already explored all values of the hypergeometric probability mass whose probabilities are smaller or equal
	// than the probability of a. The only tricky case occurs when the probability values on both sides happen to be identical. The way to handle
	// that case is by "accepting" both values simultaneously.
	pval = 0; //Accumulate probabilities in this variable
	while(a_min<a_max){
		p_left = exp(pre_comp_xterms - (loggamma[a_min] + loggamma[n-a_min] + loggamma[x-a_min] + loggamma[(N-n)-(x-a_min)]));
		p_right = exp(pre_comp_xterms - (loggamma[a_max] + loggamma[n-a_max] + loggamma[x-a_max] + loggamma[(N-n)-(x-a_max)]));
		if(p_left == p_right) {
			pval += (p_left+p_right);
			if((a==a_min) || (a==a_max)) return pval;
			a_min++; a_max--;
		}
		else if(p_left < p_right){
			pval += p_left;
			if(a==a_min) return pval;
			a_min++;
		}
		else{
			pval += p_right;
			if(a==a_max) return pval;
			a_max--;
		}
	}
	// If we get to this part of the code, it means is the mode of the distribution and, therefore, its associated p-value is 1
	return 1;
}

/* Computes the Fisher's combined p-value as a function of the margins x, n and N and the cell counts a for the K tables */
double compute_pval(long long *a, long long *x){
	long long k;
	double Tval;
	for(k=0,Tval=0; k<K; k++){
		Tval += log(fisher_pval(a[k],x[k],nt[k],Nt[k]));
	}
	return Chi2_sf(-2*Tval,2*K);
}

/* Computes the minimum attainable Fisher's combined p-value depending on the margins x, n and N for the K tables */
double compute_minpval(long long *x){
	long long k;
	double Tval;
	for(k=0,Tval=0; k<K; k++){
		Tval += logpsi[k][x[k]];
	}
	return Chi2_sf(-2*Tval,2*K);
}

/* Given the margins of the K tables and the minimum attainable Fisher's combined p-value, checks whether the interval
 * and all other intervals containing it can be pruned from the search space
 * */
int isprunable(long long *x, double pval){
	long long k;
	// If the minimum attainable p-value is below the current threshold we cannot prune the
	// interval
	if(pval <= pth) return 0;
	// If for any of the K tables, its margin x is smaller than N-n, then we cannot prune
	// the interval either
	for(k=0; k<K; k++) if(x[k] < hypercorner_bnd[k]) return 0;
	// If both checks are positive, then pruning can be applied...
	return 1;
}

void process_first_layer_pvalues(){
	long long tau, j, k, queue_idx, a;
	char *X_tr_aux;
	long long *aux_ptr;
	double pval_min_int, pval;
	// Process each length 1 interval
	for(tau=0; tau<L; tau++){
		// Direct pointer to the relevant feature vector
		X_tr_aux = X_tr[tau];
		// Compute number of 1s in the interval for each of the K tables
		for(k=0; k<K; k++){
			aux_ptr = &freq_par[tau][k];
			for(j=cum_Nt[k]; j<cum_Nt[k+1]; j++) *aux_ptr += X_tr_aux[j];
		}
		// If the interval is testable...
		// Update frequency-buckets and number of testable intervals
		pval_min_int = compute_minpval(freq_par[tau]); pmh_min_par[tau] = pval_min_int;
		if(pval_min_int <= pth){//Definition of testability in terms of critical values
			// Compute cell count for each of the K tables
			for(k=0; k<K; k++){
				at[k] = 0;
				aux_ptr = &at[k];
				for(j=cum_Nt[k]; j<cum_Nt[k+1]; j++) if(X_tr_aux[j]) *aux_ptr += Y_tr[j];
			}
			// Compute the p-value
			pval = compute_pval(at,freq_par[tau]); n_pvalues_computed++;
			// Check if the P-value is significant
			if(pval_out_file) fprintf(pval_out_file,"%lld,%lld,%e\n",l+1,tau,pval);
			if(pval <= delta_opt) { fprintf(sig_int_file,"%lld,%lld,%e\n",l+1,tau,pval); n_significant_intervals++; }
		}
		// If either the current interval or the previous one are prunable (i.e. have more than su2 ones)
		// then do NOT append the left-child to the testable queue (i.e. prune it)
		if((tau==0) || isprunable(freq_par[tau],pval_min_int) || isprunable(freq_par[tau-1],pmh_min_par[tau-1])) continue;
		// Compute index of array position to be used, wrapping around if necessary
		queue_idx = testable_queue_front + testable_queue_length;
		queue_idx = (queue_idx < L) ? queue_idx : queue_idx - L;
		// Actually append children to testable queue
		testable_queue[queue_idx] = tau-1;
		// Update queue length
		testable_queue_length++;
	}
}

void process_intervals_pvalues(){
	long long tau, j, k, queue_idx, a;
	char *X_tr_aux, *X_par_aux;
	long long *aux_ptr;
	double pval_min_int, pval;
	// While testable-interval queue is not empty, continue to process intervals
	while(testable_queue_length){
		// Pop a testable interval from the queue
		tau = testable_queue[testable_queue_front];
		testable_queue_front = (testable_queue_front<(L-1)) ? testable_queue_front + 1: 0;
		testable_queue_length--;
		// Check if we have started processing a new layer by detecting non-monotonicity in tau
		if(tau < last_tau) {
			l++;
			#ifndef NO_VERBOSE
			printf("\tProcessing layer %lld...\n",l+1);
			#endif
		}
		if((L_max>0) && ((l+1) > L_max)) {
			#ifndef NO_VERBOSE
			printf("\tMaximum interval length achieved at l=%lld. Stopping enumeration...\n",l+1);
			#endif
			break;
		}
		last_tau = tau;
		// In this case, the testable region does not change, so we don't need to check if the interval
		// has to be pruned now. If it was appended to the queue, then it has to be processed for sure
		// Compute OR and frequency of the interval for each of the K tables
		X_tr_aux = X_tr[tau+l]; X_par_aux = X_par[tau];
		for(k=0; k<K; k++){
			aux_ptr = &freq_par[tau][k];
			for(j=cum_Nt[k]; j<cum_Nt[k+1]; j++) if((!X_par_aux[j]) && X_tr_aux[j]){ X_par_aux[j] = 1; (*aux_ptr)++;}
		}
		// If the interval is testable, increase counter of testable items and frequency-buckets and
		// check if the corrected significance threshold must be reduced
		pval_min_int = compute_minpval(freq_par[tau]); pmh_min_par[tau] = pval_min_int;
		if(pval_min_int <= pth){//Definition of testability in terms of critical values
			// Compute cell count for each of the K tables
			for(k=0; k<K; k++){
				at[k] = 0;
				aux_ptr = &at[k];
				for(j=cum_Nt[k]; j<cum_Nt[k+1]; j++) if(X_par_aux[j]) *aux_ptr += Y_tr[j];
			}
			// Compute the P-value
			pval = compute_pval(at,freq_par[tau]);
			// Check if the P-value is significant
			if(pval_out_file) fprintf(pval_out_file,"%lld,%lld,%e\n",l+1,tau,pval);
			if(pval <= delta_opt) { fprintf(sig_int_file,"%lld,%lld,%e\n",l+1,tau,pval); n_significant_intervals++; }
			n_pvalues_computed++;
		}
		// If either the current interval or the previous one are prunable (i.e. have more than su2 ones)
		// then do NOT append the left-child to the testable queue (i.e. prune it)
		if((tau==0) || isprunable(freq_par[tau],pval_min_int) || isprunable(freq_par[tau-1],pmh_min_par[tau-1])) continue;
		// Compute index of array position to be used, wrapping around if necessary
		queue_idx = testable_queue_front + testable_queue_length;
		queue_idx = (queue_idx < L) ? queue_idx : queue_idx - L;
		// Actually append children to testable queue
		testable_queue[queue_idx] = tau-1;
		// Update queue length
		testable_queue_length++;
	}
}

/* Wrapper function that encapsulates the functionality required to find significant intervals */
void find_significant_intervals(){
	// Give feedback to user
	#ifndef NO_VERBOSE
	printf("\n\nSCANNING DATASET FOR SIGNIFICANT INTERVALS...\n\n");
	#endif
	// Initialise the queue as empty
	testable_queue_front = 0; testable_queue_length = 0;
	// Initialise current layer index and current number of computed p-values to 0
	l = 0; n_pvalues_computed = 0; n_significant_intervals = 0;
	// Clear the current layer frequency counters
	memset(freq_par[0],0,L*K*sizeof(long long));
	// Initialise the value of the OR vectors of current layer to original dataset
	memcpy(X_par[0],X_tr[0],L*N*sizeof(char));
	// Process the upper-most layer (i.e. the layer composed of length 1 intervals)
	#ifndef NO_VERBOSE
	printf("\tProcessing layer %lld...\n",l+1);
	#endif
	process_first_layer_pvalues();
	// Artificially initialise last_tau to L-1 to ensure the first iteration of process_intervals()
	// increases the number of layers processed if the testable queue is non-empty
	last_tau = L-1;
	// Process the rest of layers (i.e. intervals of length > 1) until the pruning naturally stops the execution
	process_intervals_pvalues();
	// Report number of significant intervals found
	fprintf(summary_file,"Number of significantly associated intervals found: %lld\n",n_significant_intervals);
}

/* -------------------FUNCTIONS TO FIND THE CORRECTED SIGNIFICANCE THRESHOLD -------------------------------------- */

/* Decrease the minimum p-value threshold one level
 */
void decrease_threshold(){
	// Remove the intervals which become untestable after the change
	m -= freq_cnt[idx_th];
	// Change threshold
	idx_th++; pth = pgrid[idx_th];
}

void process_first_layer_threshold(){
	long long tau, j, k, queue_idx;
	char *X_tr_aux;
	long long *aux_ptr;
	double pmh_min_val;
	// Process each length 1 interval
	for(tau=0; tau<L; tau++){
		n_intervals_processed++;
		// Compute number of 1s in the interval for each of the K tables
		X_tr_aux = X_tr[tau];
		for(k=0; k<K; k++){
			aux_ptr = &freq_par[tau][k];
			for(j=cum_Nt[k]; j<cum_Nt[k+1]; j++) *aux_ptr += X_tr_aux[j];
		}
		// If the interval is testable...
		// Update frequency-buckets and number of testable intervals
		pmh_min_val = compute_minpval(freq_par[tau]); pmh_min_par[tau] = pmh_min_val;
		//printf("%e, %e\n",pmh_min_val,pth);
		if(pmh_min_val <= pth){//Definition of testability in terms of critical values
			// Increase counter of appropriate bucket and overall number of testable intervals
			freq_cnt[bucket_idx(pmh_min_val)]++; m++;
			// Update threshold until FWER condition is satisfied again
			while((m*pth) > alpha) decrease_threshold();
		}
		// If either the current interval or the previous one are prunable
		// then do NOT append the left-child to the testable queue (i.e. prune it)
		if((tau==0) || isprunable(freq_par[tau],pmh_min_val) || isprunable(freq_par[tau-1],pmh_min_par[tau-1])) continue;
		// Compute index of array position to be used, wrapping around if necessary
		queue_idx = testable_queue_front + testable_queue_length;
		queue_idx = (queue_idx < L) ? queue_idx : queue_idx - L;
		// Actually append children to testable queue
		testable_queue[queue_idx] = tau-1;
		// Update queue length
		testable_queue_length++;
	}
}

void process_intervals_threshold(){
	long long tau, j, k, queue_idx;
	char *X_tr_aux, *X_par_aux;
	long long *aux_ptr;
	double pmh_min_val;
	// While testable-interval queue is not empty, continue to process intervals
	while(testable_queue_length){
		// Pop a testable interval from the queue
		tau = testable_queue[testable_queue_front];
		testable_queue_front = (testable_queue_front<(L-1)) ? testable_queue_front + 1: 0;
		testable_queue_length--;
		// Check if we have started processing a new layer by detecting non-monotonicity in tau
		if(tau < last_tau) {
			l++;
			#ifndef NO_VERBOSE
			printf("\tProcessing layer %lld...\n",l+1);
			#endif
		}
		if((L_max>0) && ((l+1) > L_max)) {
			#ifndef NO_VERBOSE
			printf("\tMaximum interval length achieved at l=%lld. Stopping enumeration...\n",l+1);
			#endif
			break;
		}
		last_tau = tau;
		// Check any of the two parents is prunable, stop processing. Notice that this check is necessary
		// even if the current interval was appended to the testable queue, because the threshold and
		// testability regions might have been modified between the time in which the current interval
		// was appended to the queue and the time in which it is being processed
		if(isprunable(freq_par[tau],pmh_min_par[tau]) || isprunable(freq_par[tau+1],pmh_min_par[tau+1])) continue;
		n_intervals_processed++;
		// Compute OR and frequency of the interval for each of the K tables
		X_tr_aux = X_tr[tau+l]; X_par_aux = X_par[tau];
		for(k=0; k<K; k++){
			aux_ptr = &freq_par[tau][k];
			for(j=cum_Nt[k]; j<cum_Nt[k+1]; j++) if((!X_par_aux[j]) && X_tr_aux[j]){ X_par_aux[j] = 1; (*aux_ptr)++;}
		}
		// If the interval is testable, increase counter of testable items and frequency-buckets and
		// check if the corrected significance threshold must be reduced
		pmh_min_val = compute_minpval(freq_par[tau]); pmh_min_par[tau] = pmh_min_val;
		if(pmh_min_val <= pth){//Definition of testability in terms of critical values
			// Increase counter of appropriate bucket and overall number of testable intervals
			freq_cnt[bucket_idx(pmh_min_val)]++; m++;
			// Update threshold until FWER condition is satisfied again
			while((m*pth) > alpha) decrease_threshold();
		}
		// If either the current interval or the previous one are prunable
		// then do NOT append the left-child to the testable queue (i.e. prune it)
		if((tau==0) || isprunable(freq_par[tau],pmh_min_val) || isprunable(freq_par[tau-1],pmh_min_par[tau-1])) continue;
		// Compute index of array position to be used, wrapping around if necessary
		queue_idx = testable_queue_front + testable_queue_length;
		queue_idx = (queue_idx < L) ? queue_idx : queue_idx - L;
		// Actually append children to testable queue
		testable_queue[queue_idx] = tau-1;
		// Update queue length
		testable_queue_length++;
	}
}

/* Function to give some feedback about the computation of the significance threshold */
void output_significance_threshold(){
	long long k;
	fprintf(summary_file,"DATASET CHARACTERISTICS:\n");
	fprintf(summary_file,"\tN = %lld, n = %lld, L = %lld\n",N,n,L);
	for(k=0; k<K; k++) fprintf(summary_file,"\t\tN[%lld] = %lld, n[%lld] = %lld\n",k,Nt[k],k,nt[k]);
	fprintf(summary_file,"RESULTS: \n");
	// Number of intervals processed, proportion of intervals pruned
	fprintf(summary_file,"Intervals processed: %lld (%f%% of total)\n",n_intervals_processed,((double)(200*n_intervals_processed))/(L*(L+1)));
	fprintf(summary_file,"Maximum testable interval length: %lld\n",l+1);
	if(L_max==0) fprintf(summary_file,"Maximum interval length to be processed: unlimited\n");
	else fprintf(summary_file,"Maximum interval length to be processed: %lld\n",L_max);
	fprintf(summary_file,"Last testability threshold: %e\n",pth);
	fprintf(summary_file,"Number of testable intervals: %lld\n",m);
	fprintf(summary_file,"Corrected significance threshold at level %e: %e\n",alpha, delta_opt);
}

/* Wrapper function that encapsulates the functionality required to find the corrected significance threshold */
void compute_corrected_significance_threshold(){
	// Give feedback to user
	#ifndef NO_VERBOSE
	printf("COMPUTING CORRECTED SIGNIFICANCE THRESHOLD...\n");
	#endif
	// Initialise the queue as empty
	testable_queue_front = 0; testable_queue_length = 0;
	// Initialise current layer index, current number of testable intervals and current number of intervals processed to 0
	l = 0; m = 0; n_intervals_processed = 0;
	// Initialise the value of the OR vectors of current layer to original dataset
	memcpy(X_par[0],X_tr[0],L*N*sizeof(char));
	// Process the upper-most layer (i.e. the layer composed of length 1 intervals)
	#ifndef NO_VERBOSE
	printf("\tProcessing layer %lld...\n",l+1);
	#endif
	process_first_layer_threshold();
	// Artificially initialise last_tau to L-1 to ensure the first iteration of process_intervals()
	// increases the number of layers processed if the testable queue is non-empty
	last_tau = L-1;
	// Process the rest of layers (i.e. intervals of length > 1) until the pruning naturally stops the execution
	process_intervals_threshold();
	// Set final corrected significance threshold
	delta_opt = alpha/m;
	// Print results to stdout
	output_significance_threshold();
}

/*--------------------------------------------------------- FILE I/O --------------------------------------------------------*/
/* Do a first scan of the file containing the class labels to compute the total number of observations, N,
 * and the total number of observations in the positive class, n
 * */
void get_N_n(char *labels_file){
	FILE *f_labels;//Stream with file containing class labels
	int n_read;//Number of chars read
	int i;// Iterator variable to be used in loops
	char char_to_int[256];//Array for converting chars to int fast
	char *read_buf, *read_buf_aux, *read_buf_end;//Buffer for reading from file and extra pointers for loops

	// Initialise both counters to 0 (the variables are defined as global variables in wy.c)
	N = 0; n = 0;

	//Try to open file, giving an error message if it fails
	if(!(f_labels = fopen(labels_file,"r"))){
		fprintf(stderr, "Error in function get_N_n when opening file %s\n",labels_file);
		exit(1);
	}

	//Try to allocate memory for the buffer, giving an error message if it fails
	read_buf = (char *)malloc(READ_BUF_SIZ*sizeof(char));
	if(!read_buf){
		fprintf(stderr,"Error in function get_N_n: couldn't allocate memory for array read_buf\n");
		exit(1);
	}

	//Initialize the char to int converter
	for(i=0;i<256;i++) char_to_int[i] = 127;
	// We only care about the chars '0' and '1'. Everything else is mapped into the same "bucket"
	char_to_int['0'] = 0; char_to_int['1'] = 1;

	// Read the entire file
	while(1){
		// Try to read READ_BUF_SIZ chars from the file containing the class labels
		n_read = fread(read_buf,sizeof(char),READ_BUF_SIZ,f_labels);
		// If the number of chars read, n_read_ is smaller than READ_BUF_SIZ, either the file ended
		// or there was an error. Check if it was the latter
		if((n_read < READ_BUF_SIZ) && !feof(f_labels)){
			fprintf(stderr,"Error in function get_N_n while reading the file %s\n",labels_file);
			exit(1);
		}
		// Process the n_read chars read from the file
		for(read_buf_aux=read_buf,read_buf_end=read_buf+n_read;read_buf_aux<read_buf_end;read_buf_aux++){
			//If the character is anything other than '0' or '1' go to process the next char
			if(char_to_int[*read_buf_aux] == 127) continue;
			N++;
			if(char_to_int[*read_buf_aux]) n++;
		}
		// Check if the file ended,. If yes, then exit the while loop
		if(feof(f_labels)) break;
	}

	//Close the file
	fclose(f_labels);

	//Free allocated memory
	free(read_buf);
}

void read_labels_file(char *labels_file){
	FILE *f_labels;//Stream with file containing class labels
	int n_read;//Number of chars read
	long long i;// Iterator variable to be used in loops
	long long k; //Current table index
	char char_to_int[256];//Array for converting chars to int fast
	char *read_buf, *read_buf_aux, *read_buf_end;//Buffer for reading from file and extra pointers for loops
	char *labels_aux = Y_tr;//Auxiliary pointer to array labels for increments

	//Try to open file, giving an error message if it fails
	if(!(f_labels = fopen(labels_file,"r"))){
		fprintf(stderr, "Error in function read_labels_file when opening file %s\n",labels_file);
		exit(1);
	}

	//Try to allocate memory for the buffer, giving an error message if it fails
	read_buf = (char *)malloc(READ_BUF_SIZ*sizeof(char));
	if(!read_buf){
		fprintf(stderr,"Error in function read_labels_file: couldn't allocate memory for array read_buf\n");
		exit(1);
	}

	//Initialize the char to int converter
	for(i=0;i<256;i++) char_to_int[i] = 127;
	// We only care about the chars '0' and '1'. Everything else is mapped into the same "bucket"
	char_to_int['0'] = 0; char_to_int['1'] = 1;

	// Read the entire file
	i = 0; //Here i stands for the number of labels read so far
	k = 0; //Assume all observations have been ordered by table in the input file!!!
	while(1){
		// Try to read READ_BUF_SIZ chars from the file containing the class labels
		n_read = fread(read_buf,sizeof(char),READ_BUF_SIZ,f_labels);
		// If the number of chars read, n_read_ is smaller than READ_BUF_SIZ, either the file ended
		// or there was an error. Check if it was the latter
		if((n_read < READ_BUF_SIZ) && !feof(f_labels)){
			fprintf(stderr,"Error in function read_labels_file while reading the file %s\n",labels_file);
			exit(1);
		}
		// Process the n_read chars read from the file
		for(read_buf_aux=read_buf,read_buf_end=read_buf+n_read;read_buf_aux<read_buf_end;read_buf_aux++){
			//If the character is anything other than '0' or '1' go to process the next char
			if(char_to_int[*read_buf_aux] == 127) continue;
			*labels_aux++ = char_to_int[*read_buf_aux];
			nt[k] += char_to_int[*read_buf_aux];
			i++;
			if(i==cum_Nt[k+1]) k++;
		}
		// Check if the file ended,. If yes, then exit the while loop
		if(feof(f_labels)) break;
	}

	// Sanity check to see if we successfully read the correct number of labels
	i = labels_aux-Y_tr;
	if(i != N){
		fprintf(stderr,"Error in function read_labels_file: incorrect number of labels read. Read %lld, correct number %lld\n",i,N);
		exit(1);
	}

	//Close the file
	fclose(f_labels);

	//Free allocated memory
	free(read_buf);
}

void get_L(char *filename){
	FILE *f_dat = ((FILE*)0);
	int i, j, n_read;
	char char_to_int[256];//Array for converting chars to int fast
	char *read_buf, *read_buf_aux, *read_buf_end;//Buffer for reading from file and extra pointers for loops

	//Try to open file, giving an error message if it fails
	if(!(f_dat = fopen(filename,"r"))){
		fprintf(stderr, "Error in function get_L when opening file %s\n",filename);
		exit(1);
	}
	//Try to allocate memory for the buffer, giving an error message if it fails
	read_buf = (char *)malloc(READ_BUF_SIZ*sizeof(char));
	if(!read_buf){
		fprintf(stderr,"Error in function get_L: couldn't allocate memory for array read_buf\n");
		exit(1);
	}

	//Initialize the char to int converter
	for(i=0;i<256;i++) char_to_int[i] = 0;
	// We only care about the chars '0' and '1'. Everything else is mapped into the same "bucket"
	char_to_int['\n'] = 1;

	// Read the entire file, counting the number of lines
	L = 0;
	while(1){
		// Try to read READ_BUF_SIZ chars from the file containing the class labels
		n_read = fread(read_buf,sizeof(char),READ_BUF_SIZ,f_dat);
		// If the number of chars read, n_read_ is smaller than READ_BUF_SIZ, either the file ended
		// or there was an error. Check if it was the latter
		if((n_read < READ_BUF_SIZ) && !feof(f_dat)){
			fprintf(stderr,"Error in function get_L while reading the file %s\n",filename);
			exit(1);
		}
		// Process the n_read chars read from the file
		for(read_buf_aux=read_buf,read_buf_end=read_buf+n_read;read_buf_aux<read_buf_end;read_buf_aux++) if(char_to_int[*read_buf_aux]) L++;
		// Check if the file ended,. If yes, then exit the while loop
		if(feof(f_dat)) break;
	}

	// Close file
	fclose(f_dat);
	// Free allocated memory
	free(read_buf);
}


void read_dataset_file(char *filename, char *ptr){
	FILE *f_dat = ((FILE*)0);
	int i, j, n_read;
	char char_to_int[256];//Array for converting chars to int fast
	char *read_buf, *read_buf_aux, *read_buf_end;//Buffer for reading from file and extra pointers for loops

	//Try to open file, giving an error message if it fails
	if(!(f_dat = fopen(filename,"r"))){
		fprintf(stderr, "Error in function read_dataset_file when opening file %s\n",filename);
		exit(1);
	}
	//Try to allocate memory for the buffer, giving an error message if it fails
	read_buf = (char *)malloc(READ_BUF_SIZ*sizeof(char));
	if(!read_buf){
		fprintf(stderr,"Error in function read_dataset_file: couldn't allocate memory for array read_buf\n");
		exit(1);
	}

	//Initialize the char to int converter
	for(i=0;i<256;i++) char_to_int[i] = 127;
	// We only care about the chars '0' and '1'. Everything else is mapped into the same "bucket"
	char_to_int['0'] = 0; char_to_int['1'] = 1;

	// Read the entire file
	while(1){
		// Try to read READ_BUF_SIZ chars from the file containing the class labels
		n_read = fread(read_buf,sizeof(char),READ_BUF_SIZ,f_dat);
		// If the number of chars read, n_read_ is smaller than READ_BUF_SIZ, either the file ended
		// or there was an error. Check if it was the latter
		if((n_read < READ_BUF_SIZ) && !feof(f_dat)){
			fprintf(stderr,"Error in function read_dataset_file while reading the file %s\n",filename);
			exit(1);
		}
		// Process the n_read chars read from the file
		for(read_buf_aux=read_buf,read_buf_end=read_buf+n_read;read_buf_aux<read_buf_end;read_buf_aux++){
			//If the character is anything other than '0' or '1' go to process the next char
			if(char_to_int[*read_buf_aux] == 127) continue;
			*ptr++ = char_to_int[*read_buf_aux];
		}
		// Check if the file ended,. If yes, then exit the while loop
		if(feof(f_dat)) break;
	}

	// Close file
	fclose(f_dat);
	// Free allocated memory
	free(read_buf);
}

/* Do a first scan of the file containing the number of observations per table in order to compute the number of tables
 * */
void get_K(char *covariates_file){
	FILE *f_covariates;//Stream with file containing class labels
	int n_read;//Number of chars read
	int i;// Iterator variable to be used in loops
	char char_to_int[256];//Array for converting chars to int fast
	char *read_buf, *read_buf_aux, *read_buf_end;//Buffer for reading from file and extra pointers for loops

	// Initialise both counters to 0 (the variables are defined as global variables in wy.c)
	K = 0;

	//Try to open file, giving an error message if it fails
	if(!(f_covariates = fopen(covariates_file,"r"))){
		fprintf(stderr, "Error in function get_K when opening file %s\n",covariates_file);
		exit(1);
	}

	//Try to allocate memory for the buffer, giving an error message if it fails
	read_buf = (char *)malloc(READ_BUF_SIZ*sizeof(char));
	if(!read_buf){
		fprintf(stderr,"Error in function get_K: couldn't allocate memory for array read_buf\n");
		exit(1);
	}

	//Initialize the char to int converter
	for(i=0;i<256;i++) char_to_int[i] = 127;
	// We only care about newlines
	char_to_int['\n'] = 0;

	// Read the entire file, counting the number of lines
	while(1){
		// Try to read READ_BUF_SIZ chars from the file containing the class labels
		n_read = fread(read_buf,sizeof(char),READ_BUF_SIZ,f_covariates);
		// If the number of chars read, n_read_ is smaller than READ_BUF_SIZ, either the file ended
		// or there was an error. Check if it was the latter
		if((n_read < READ_BUF_SIZ) && !feof(f_covariates)){
			fprintf(stderr,"Error in function get_K while reading the file %s\n",covariates_file);
			exit(1);
		}
		// Process the n_read chars read from the file
		for(read_buf_aux=read_buf,read_buf_end=read_buf+n_read;read_buf_aux<read_buf_end;read_buf_aux++){
			//If the character is not a newline process the next character
			if(char_to_int[*read_buf_aux] == 127) continue;
			K++;
		}
		// Check if the file ended,. If yes, then exit the while loop
		if(feof(f_covariates)) break;
	}

	//Close the file
	fclose(f_covariates);

	//Free allocated memory
	free(read_buf);
}

void read_covariates_file(char *covariates_file){
	FILE *f_covariates;//Stream with file containing class labels
	int n_read;//Number of chars read
	long long i;// Iterator variable to be used in loops
	long long k;//Number of tables already processed
	char c;// Iterator variable to be used in loops
	char char_to_int[256];//Array for converting chars to int fast
	char *read_buf, *read_buf_aux, *read_buf_end;//Buffer for reading from file and extra pointers for loops
	char *labels_aux = Y_tr;//Auxiliary pointer to array labels for increments

	//Try to open file, giving an error message if it fails
	if(!(f_covariates = fopen(covariates_file,"r"))){
		fprintf(stderr, "Error in function read_covariates_file when opening file %s\n",covariates_file);
		exit(1);
	}

	//Try to allocate memory for the buffer, giving an error message if it fails
	read_buf = (char *)malloc(READ_BUF_SIZ*sizeof(char));
	if(!read_buf){
		fprintf(stderr,"Error in function read_covariates_file: couldn't allocate memory for array read_buf\n");
		exit(1);
	}

	//Initialize the char to int converter
	for(i=0;i<256;i++) char_to_int[i] = 127;
	// We only care about chars representing digits and newline
	for(c='0'; c<='9'; c++) char_to_int[c] = c - '0';
	char_to_int['\n'] = 126;

	// Read the entire file
	i = 0; k = 0;
	while(1){
		// Try to read READ_BUF_SIZ chars from the file containing the class labels
		n_read = fread(read_buf,sizeof(char),READ_BUF_SIZ,f_covariates);
		// If the number of chars read, n_read_ is smaller than READ_BUF_SIZ, either the file ended
		// or there was an error. Check if it was the latter
		if((n_read < READ_BUF_SIZ) && !feof(f_covariates)){
			fprintf(stderr,"Error in function read_covariates_file while reading the file %s\n",covariates_file);
			exit(1);
		}
		// Process the n_read chars read from the file
		for(read_buf_aux=read_buf,read_buf_end=read_buf+n_read;read_buf_aux<read_buf_end;read_buf_aux++){
			//If the character is neither a digit nor a newline process the next char
			if(char_to_int[*read_buf_aux] == 127) continue;
			// If the character is a newline, we have read a number already so we save it and go to next line
			if(char_to_int[*read_buf_aux] == 126){
				Nt[k++] = i;
				cum_Nt[k] = cum_Nt[k-1] + Nt[k-1];
				i = 0;
				continue;
			}
			// Otherwise the character is a digit, so we accumulate it into the current number
			i = 10*i + char_to_int[*read_buf_aux];
		}
		// Check if the file ended,. If yes, then exit the while loop
		if(feof(f_covariates)) break;
	}

	// Sanity check to see if we successfully read the distribution of observations per table
	i = 0;
	for(k=0; k<K; k++) i += Nt[k];
	if(i != N){
		fprintf(stderr,"Error in function read_covariates_file: incorrect number of observations per table read. Total N %lld, Accumulated N in covariates file %lld\n",N,i);
		exit(1);
	}

	//Close the file
	fclose(f_covariates);

	//Free allocated memory
	free(read_buf);
}

inline int bucket_idx(double pval){
	int idx;
	idx = (int)floor(-log10(pval)/log10_p_step);
	if(idx<0) idx = 0;
	if(idx>NGRID) idx = NGRID;
	return idx;
}

/* ----------------------------------------------------ENTRY POINT---------------------------------------------------------- */

int main(int argc, char *argv[]){
	char *tmp_filename;
	char *R_command;
    char i;
    char n_fixed_args = 0;
    char idx_fixed_args[6] = {-1,-1,-1,-1,-1,-1};
    char idx_postprocessing_folder = -1, idx_out_pvals_file = -1;

    // Process input
    for(i=1; i<argc; i++){
        if((strcmp(argv[i],"-postprocessing_folder")==0) || (strcmp(argv[i],"-pp")==0)) idx_postprocessing_folder = ++i;
        else if((strcmp(argv[i],"-pval_file")==0)) idx_out_pvals_file = ++i;
        else idx_fixed_args[n_fixed_args++] = i;
    }

	// Check input
	if(n_fixed_args != 6){
		printf("ERROR: INCORRECT SYNTAX!\n");
		printf("\tUSAGE: ./program_name X_file Y_file C_file alpha L_max base_filename [-postprocessing_folder path_to_pyfiltering.py] [-pval_file all_pvals_file]\n");
		exit(1);
	}

	// Get time when program started to run
	t_init = measureTime();

	// INITIALISATION
	tic = measureTime();

	// First allocate memory for filename holder
	tmp_filename = (char *)malloc((strlen(argv[idx_fixed_args[5]])+512)*sizeof(char));

	// Create a file to report significant intervals
	strcpy(tmp_filename,argv[idx_fixed_args[5]]); strcat(tmp_filename,"_sigints.csv");
	if(!(sig_int_file = fopen(tmp_filename,"w"))){
		fprintf(stderr, "Error in function main when opening file %s\n",tmp_filename);
		exit(1);
	}
	// If the file was successfully create, write the file header
	fprintf(sig_int_file,"l,tau,P-value\n");

	// Create a file to report runtime information
	strcpy(tmp_filename,argv[idx_fixed_args[5]]); strcat(tmp_filename,"_timing.txt");
	if(!(timing_file = fopen(tmp_filename,"w"))){
		fprintf(stderr, "Error in function main when opening file %s\n",tmp_filename);
		exit(1);
	}

	// Create a file to report varied information such as number of intervals processed, etc.
	strcpy(tmp_filename,argv[idx_fixed_args[5]]); strcat(tmp_filename,"_summary.txt");
	if(!(summary_file = fopen(tmp_filename,"w"))){
		fprintf(stderr, "Error in function main when opening file %s\n",tmp_filename);
		exit(1);
	}
    
	// Create a file to report histogram of maximum attainable Fisher combined statistics (only reliable in the testable range)
	strcpy(tmp_filename,argv[idx_fixed_args[5]]); strcat(tmp_filename,"_maxfish_hist.txt");
	if(!(maxfish_hist_file = fopen(tmp_filename,"w"))){
		fprintf(stderr, "Error in function main when opening file %s\n",tmp_filename);
		exit(1);
	}

	// Free filename holder
	free(tmp_filename);

	// If the optional argument was used, create a file to output all testable P-values
	pval_out_file = ((FILE *)0);
	if(idx_out_pvals_file != -1){
		// Actually try to create file, if that fails raise an error
		if(!(pval_out_file = fopen(argv[idx_out_pvals_file],"w"))){
			fprintf(stderr, "Error in function main when opening file %s\n",argv[idx_out_pvals_file]);
			exit(1);
		}
		// If the file was successfully create, write the file header
		fprintf(pval_out_file,"l,tau,P-value\n");
	}

	// Initialise main variables
	sis_init(argv[idx_fixed_args[0]], argv[idx_fixed_args[1]], argv[idx_fixed_args[2]], atof(argv[idx_fixed_args[3]]), atoll(argv[idx_fixed_args[4]]));

	toc = measureTime();
	time_initialisation = toc-tic;

	// Main functionality
	tic = measureTime();
	compute_corrected_significance_threshold();
	toc = measureTime();
	time_comp_threshold = toc-tic;
	tic = measureTime();
	find_significant_intervals();
	toc = measureTime();
	time_comp_significant_intervals = toc-tic;

	// Get time when program finished
	t_end = measureTime();

	// Produce output and free memory
	sis_end();

	// Call Python postprocessing script
	R_command = (char *)malloc((strlen(argv[idx_fixed_args[5]])+512)*sizeof(char));
	strcpy(R_command,"python ");
    if(idx_postprocessing_folder != -1) strcat(R_command,argv[idx_postprocessing_folder]);
    strcat(R_command,"pyfiltering_meta.py "); strcat(R_command,argv[idx_fixed_args[5]]);
	strcat(R_command,"_sigints.csv "); strcat(R_command,argv[idx_fixed_args[5]]); strcat(R_command,"_sigints_filtered.corrected.csv");
	system(R_command);
	free(R_command);

	// Return
	exit(0);
}

#endif
