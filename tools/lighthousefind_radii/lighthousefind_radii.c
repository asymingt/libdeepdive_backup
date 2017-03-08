#include <stdio.h>
#include <stdlib.h>
#include "linmath.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

#define PTS 32
#define MAX_CHECKS 40000
#define MIN_HITS_FOR_VALID 10

FLT hmd_points[PTS*3];
FLT hmd_norms[PTS*3];
FLT hmd_point_angles[PTS*2];
int hmd_point_counts[PTS*2];
int best_hmd_target = 0;
int LoadData( char Camera, const char * FileData );

//Values used for RunTest()
FLT LighthousePos[3] = { 0, 0, 0 };
FLT LighthouseQuat[4] = { 1, 0, 0, 0 };

FLT RunTest( int print );
void PrintOpti();

typedef struct
{
	FLT x;
	FLT y;
	FLT z;
} Point;

typedef struct
{
	unsigned char index1;
	unsigned char index2;
	FLT KnownDistance;
} PointPair;

//typedef struct
//{
//	FLT radius;
//	FLT HorizAngle;
//	FLT VertAngle;
//} RadiusGuess;

typedef struct
{
	FLT HorizAngle;
	FLT VertAngle;
} SensorAngles;

#define SQUARED(x) ((x)*(x))

FLT calculateFitness(SensorAngles *angles, FLT *radii, PointPair *pairs, size_t numPairs)
{
	FLT fitness = 0;
	for (size_t i = 0; i < numPairs; i++)
	{
		FLT estimatedDistanceBetweenPoints =
			SQUARED(radii[pairs[i].index1])
			+ SQUARED(radii[pairs[i].index2])
			- 2 * radii[pairs[i].index1] * radii[pairs[i].index2]
				* FLT_SIN(angles[pairs[i].index1].HorizAngle) * FLT_SIN(angles[pairs[i].index2].HorizAngle)
				* FLT_COS(angles[pairs[i].index1].VertAngle - angles[pairs[i].index2].VertAngle)
			+ FLT_COS(angles[pairs[i].index1].VertAngle) * FLT_COS(angles[pairs[i].index2].VertAngle);
		fitness += SQUARED(estimatedDistanceBetweenPoints);
	}

	return FLT_SQRT(fitness);
}

#define MAX_RADII 32

// note gradientOut will be of the same degree as numRadii
void getGradient(FLT *gradientOut, SensorAngles *angles, FLT *radii, size_t numRadii, PointPair *pairs, size_t numPairs, const FLT precision)
{
	FLT baseline = calculateFitness(radii, pairs, numPairs);

	for (size_t i = 0; i++; i < numRadii)
	{
		FLT tmpPlus[MAX_RADII];
		memcpy(tmpPlus, radii, sizeof(&radii) * numRadii);
		tmpPlus[i] += precision;
		gradientOut[i] = calculateFitness(angles, tmpPlus, pairs, numPairs) - baseline;
	}

	return;
}

void normalizeAndMultiplyVector(FLT *vectorToNormalize, size_t count, FLT desiredMagnitude)
{
	FLT distanceIn = 0;
	
	for (size_t i = 0; i < count; i++)
	{
		distanceIn += SQUARED(vectorToNormalize[i]);
	}
	distanceIn = FLT_SQRT(distanceIn);


	FLT scale = desiredMagnitude / distanceIn;

	for (size_t i = 0; i < count; i++)
	{
		vectorToNormalize[i] *= scale;
	}

	return;
}


static RefineEstimateUsingGradientDescent(FLT *estimateOut, SensorAngles *angles, FLT *initialEstimate, size_t numRadii, PointPair *pairs, size_t numPairs, FILE *logFile)
{
	int i = 0;
	FLT lastMatchFitness = calculateFitness(angles, initialEstimate, pairs, numPairs);
	memcpy(estimateOut, initialEstimate, sizeof(&estimateOut) * numRadii);


	// The values below are somewhat magic, and definitely tunable
	// The initial vlue of g will represent the biggest step that the gradient descent can take at first.
	//   bigger values may be faster, especially when the initial guess is wildly off.
	//   The downside to a bigger starting guess is that if we've picked a good guess at the local minima
	//   if there are other local minima, we may accidentally jump to such a local minima and get stuck there.
	//   That's fairly unlikely with the lighthouse problem, from expereince.
	//   The other downside is that if it's too big, we may have to spend a few iterations before it gets down
	//   to a size that doesn't jump us out of our minima.
	// The terminal value of g represents how close we want to get to the local minima before we're "done"
	// The change in value of g for each iteration is intentionally very close to 1.
	//   in fact, it probably could probably be 1 without any issue.  The main place where g is decremented
	//   is in the block below when we've made a jump that results in a worse fitness than we're starting at.
	//   In those cases, we don't take the jump, and instead lower the value of g and try again.
	for (FLT g = 0.4; g > 0.01; g *= 0.99)
	{
		i++;

		

		FLT point1[MAX_RADII];
		memcpy(point1, estimateOut, sizeof(&point1) * numRadii);

		// let's get 3 iterations of gradient descent here.
		FLT gradient1[MAX_RADII];
		getGradient(&gradient1, angles, point1, numRadii, pairs, numPairs, g / 1000 /*somewhat arbitrary*/);
		normalizeAndMultiplyVector(gradient1, numRadii, g);

		FLT point2[MAX_RADII];
		for (size_t i = 0; i < numRadii; i++)
		{
			point2[i] = point1[i] + gradient1[i];
		}
		FLT gradient2[MAX_RADII];
		getGradient(&gradient2, angles, point2, numRadii, pairs, numPairs, g / 1000 /*somewhat arbitrary*/);
		normalizeAndMultiplyVector(gradient2, numRadii, g);

		FLT point3[MAX_RADII];
		for (size_t i = 0; i < numRadii; i++)
		{
			point3[i] = point2[i] + gradient2[i];
		}

		// remember that gradient descent has a tendency to zig-zag when it encounters a narrow valley?
		// Well, solving the lighthouse problem presents a very narrow valley, and the zig-zag of a basic
		// gradient descent is kinda horrible here.  Instead, think about the shape that a zig-zagging 
		// converging gradient descent makes.  Instead of using the gradient as the best indicator of 
		// the direction we should follow, we're looking at one side of the zig-zag pattern, and specifically
		// following *that* vector.  As it turns out, this works *amazingly* well.  

		FLT specialGradient[MAX_RADII];
		for (size_t i = 0; i < numRadii; i++)
		{
			specialGradient[i] = point3[i] - gradient1[i];
		}

		// The second parameter to this function is very much a tunable parameter.  Different values will result
		// in a different number of iterations before we get to the minimum.  Numbers between 3-10 seem to work well
		// It's not clear what would be optimum here.
		normalizeAndMultiplyVector(specialGradient, numRadii, g/4);


		FLT point4[MAX_RADII];
		for (size_t i = 0; i < numRadii; i++)
		{
			point4[i] = point3[i] + specialGradient[i];
		}


		FLT newMatchFitness = calculateFitness(angles, point4, pairs, numPairs);

		if (newMatchFitness > lastMatchFitness)
		{
			//if (logFile)
			//{
			//	writePoint(logFile, lastPoint.x, lastPoint.y, lastPoint.z, 0xFFFFFF);
			//}

			lastMatchFitness = newMatchFitness;
			memcpy(estimateOut, point4, sizeof(&lastPoint) * numRadii);

#ifdef RADII_DEBUG
			printf("+");
#endif
		}
		else
		{
#ifdef RADII_DEBUG
			printf("-");
#endif
			// if it wasn't a match, back off on the distance we jump
			g *= 0.7;

		}


	}
	printf("\ni=%d\n", i);
}


int main( int argc, char ** argv )
{

	if( argc != 3 )
	{
		fprintf( stderr, "Error: usage: camfind [camera (L or R)] [datafile]\n" );
		exit( -1 );
	}

	//Load either 'L' (LH1) or 'R' (LH2) data.
	if( LoadData( argv[1][0], argv[2] ) ) return 5;




	
}





int LoadData( char Camera, const char * datafile  )
{

	//First, read the positions of all the sensors on the HMD.
	FILE * f = fopen( "HMD_points.csv", "r" );
	int pt = 0;
	if( !f ) { fprintf( stderr, "error: can't open hmd points.\n" ); return -5; }
	while(!feof(f) && !ferror(f) && pt < PTS)
	{
		float fa, fb, fc;
		int r = fscanf( f,"%g %g %g\n", &fa, &fb, &fc );
		hmd_points[pt*3+0] = fa;
		hmd_points[pt*3+1] = fb;
		hmd_points[pt*3+2] = fc;
		pt++;
		if( r != 3 )
		{
			fprintf( stderr, "Not enough entries on line %d of points\n", pt );
			return -8;
		}
	}
	if( pt < PTS )
	{
		fprintf( stderr, "Not enough points.\n" );
		return -9;
	}
	fclose( f );
	printf( "Loaded %d points\n", pt );

	//Read all the normals on the HMD into hmd_norms.
	f = fopen( "HMD_normals.csv", "r" );
	int nrm = 0;
	if( !f ) { fprintf( stderr, "error: can't open hmd points.\n" ); return -5; }
	while(!feof(f) && !ferror(f) && nrm < PTS)
	{
		float fa, fb, fc;
		int r = fscanf( f,"%g %g %g\n", &fa, &fb, &fc );
		hmd_norms[nrm*3+0] = fa;
		hmd_norms[nrm*3+1] = fb;
		hmd_norms[nrm*3+2] = fc;
		nrm++;
		if( r != 3 )
		{
			fprintf( stderr, "Not enough entries on line %d of normals\n", nrm );
			return -8;
		}
	}
	if( nrm < PTS )
	{
		fprintf( stderr, "Not enough points.\n" );
		return -9;
	}
	if( nrm != pt )
	{
		fprintf( stderr, "point/normal counts disagree.\n" );
		return -9;
	}
	fclose( f );
	printf( "Loaded %d norms\n", nrm );

	//Actually load the processed data!
	int xck = 0;
	f = fopen( datafile, "r" );
	if( !f )
	{
		fprintf( stderr, "Error: cannot open %s\n", datafile );
		exit (-11);
	}
	int lineno = 0;
	while( !feof( f ) )
	{
		//Format:
		// HMD LX 0 3433 173656.227498 327.160210 36.342361 2.990936
		lineno++;
		char devn[10];
		char inn[10];
		int id;
		int pointct;
		FLT avgTime;
		FLT avgLen;
		FLT stddevTime;
		FLT stddevLen;
		int ct = fscanf( f, "%9s %9s %d %d %lf %lf %lf %lf\n", devn, inn, &id, &pointct, &avgTime, &avgLen, &stddevTime, &stddevLen );
		if( ct == 0 ) continue;
		if( ct != 8 )
		{
			fprintf( stderr, "Malformatted line, %d in processed_data.txt\n", lineno );
		}
		if( strcmp( devn, "HMD" ) != 0 ) continue;

		if( inn[0] != Camera ) continue;

		int isy = inn[1] == 'Y';

		hmd_point_angles[id*2+isy] = ( avgTime - 200000 ) / 200000 * 3.1415926535/2.0;
		hmd_point_counts[id*2+isy] = pointct;
	}
	fclose( f );


	int targpd;
	int maxhits = 0;

	for( targpd = 0; targpd < PTS; targpd++ )
	{
		int hits = hmd_point_counts[targpd*2+0];
		if( hits > hmd_point_counts[targpd*2+1] ) hits = hmd_point_counts[targpd*2+1];
		//Need an X and a Y lock.  

		if( hits > maxhits ) { maxhits = hits; best_hmd_target = targpd; }
	}
	if( maxhits < MIN_HITS_FOR_VALID )
	{
		fprintf( stderr, "Error: Not enough data for a primary fix.\n" );
	}

	return 0;
}

