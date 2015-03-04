#include <vtkm/Pair.h>
#include <vtkm/Types.h>
#include <vtkm/worklet/DispatcherMapField.h>
#include <vtkm/worklet/WorkletMapField.h>

#include <iostream>

namespace worklets
{
//now that the device adapter is included set a global typedef
//that is the chosen device tag
typedef VTKM_DEFAULT_DEVICE_ADAPTER_TAG DeviceAdapter;

/// Linear interpolation
template <typename T1, typename T2>
VTKM_EXEC_EXPORT
T1 lerp(T1 a, T1 b, T2 t)
{
  return a + t*(b-a);
}

template< typename FieldType>
struct PortalTypes
{
public:
  typedef vtkm::cont::ArrayHandle<FieldType> HandleType;
  typedef typename HandleType::template ExecutionTypes<DeviceAdapter> ExecutionTypes;

  typedef typename ExecutionTypes::Portal Portal;
  typedef typename ExecutionTypes::PortalConst PortalConst;
};

/// \brief Computes Marching Cubes case number for each cell, along with the number of vertices generated by that case
///
template <typename FieldType, typename CountType, typename VertNumType, int NumCellsToFuse>
class FusedClassifyCell : public vtkm::worklet::WorkletMapField
{
public:
  typedef void ControlSignature(FieldIn<IdType> inputCellId, FieldOut<AllTypes> hasOutput, FieldOut<AllTypes> numVertsOut);
  typedef _3 ExecutionSignature(_1, _2);
  typedef _1 InputDomain;

  typedef typename PortalTypes<FieldType>::PortalConst FieldPortalType;
  FieldPortalType pointData;

  float isovalue;
  int xdim, ydim, zdim;
  int cellsPerLayer;
  int pointsPerLayer;

  template<typename T>
  VTKM_CONT_EXPORT
  FusedClassifyCell(const T& pointHandle,
                    float iso,
                    int dims[3]) :
         pointData( pointHandle.PrepareForInput( DeviceAdapter() ) ),
         isovalue( iso ),
         xdim(dims[0]),
         ydim(dims[1]),
         zdim(dims[2]),
         cellsPerLayer((xdim - 1) * (ydim - 1)),
         pointsPerLayer (xdim*ydim)
   {

   }

  VTKM_EXEC_EXPORT
  VertNumType operator()(vtkm::Id firstCellId, CountType& hasOutput) const
  {
    //compute the first x,y,z before we loop.
    int i0 = 0;
    {
      const vtkm::Id cellId = (firstCellId * NumCellsToFuse);
      const int x = cellId % (xdim - 1);
      const int y = (cellId / (xdim - 1)) % (ydim -1);
      const int z = cellId / cellsPerLayer;
      i0 = x + y*xdim + z * pointsPerLayer;
    }

    // Compute indices for the vertices of this cell
    int i1 = i0   + 1;
    int i2 = i0   + 1 + xdim;
    int i3 = i0   + xdim;
    int i4 = i0   + pointsPerLayer;
    int i5 = i1   + pointsPerLayer;
    int i6 = i2   + pointsPerLayer;
    int i7 = i3   + pointsPerLayer;

    // Get the field values at the vertices
    float f0 = this->pointData.Get(i0);
    float f1 = this->pointData.Get(i1);
    float f2 = this->pointData.Get(i2);
    float f3 = this->pointData.Get(i3);
    float f4 = this->pointData.Get(i4);
    float f5 = this->pointData.Get(i5);
    float f6 = this->pointData.Get(i6);
    float f7 = this->pointData.Get(i7);

    unsigned int cubeindex = (f0 > isovalue);
    cubeindex += (f1 > isovalue)*2;
    cubeindex += (f2 > isovalue)*4;
    cubeindex += (f3 > isovalue)*8;
    cubeindex += (f4 > isovalue)*16;
    cubeindex += (f5 > isovalue)*32;
    cubeindex += (f6 > isovalue)*64;
    cubeindex += (f7 > isovalue)*128;
    VertNumType vertCount = numVerticesTable[cubeindex];

    //handle if we are fusing multiple cells now
    for(int i=1; i < NumCellsToFuse; ++i)
      {
      //update the left hand verts to be the old right hand verts
      //shape of voxel back face is:
      // 7 6
      // 4 5
      //shape of voxel front face is:
      // 3 2
      // 0 1

      i0 = i1;
      i3 = i2;
      i4 = i5;
      i7 = i6;

      //update the last 4 verts to be new values
      ++i1;
      ++i2;
      ++i5;
      ++i6;

      f0 = f1;
      f1 = this->pointData.Get(i1);
      f3 = f2;
      f2 = this->pointData.Get(i2);
      f4 = f5;
      f5 = this->pointData.Get(i5);
      f7 = f6;
      f6 = this->pointData.Get(i6);

      // Compute the Marching Cubes case number for this cell
      cubeindex =  (f0 > isovalue);
      cubeindex += (f1 > isovalue)*2;
      cubeindex += (f2 > isovalue)*4;
      cubeindex += (f3 > isovalue)*8;
      cubeindex += (f4 > isovalue)*16;
      cubeindex += (f5 > isovalue)*32;
      cubeindex += (f6 > isovalue)*64;
      cubeindex += (f7 > isovalue)*128;

      //saving number of triangles not number of verts
      vertCount += numVerticesTable[cubeindex];
      }

    // Return the number of triangles this case generates
    hasOutput = (vertCount == 0) ? 0 : 1;
    return vertCount;
  }
};


/// \brief Compute isosurface vertices, and scalars
///
template <typename FieldType, typename OutputType, int NumCellsToFuse>
class IsosurfaceFusedUniformGridFunctor : public vtkm::worklet::WorkletMapField
{
public:
  typedef void ControlSignature(FieldIn<IdType> inputCellId);
  typedef void ExecutionSignature(_1);
  typedef _1 InputDomain;

  typedef typename PortalTypes< vtkm::Id >::PortalConst IdPortalType;
  IdPortalType outputVerticesLoc;

  typedef typename PortalTypes< FieldType >::PortalConst FieldPortalType;
  FieldPortalType field, source;

  typedef typename PortalTypes< OutputType >::Portal ScalarPortalType;
  ScalarPortalType scalars;

  typedef typename PortalTypes< vtkm::Vec<OutputType,3> >::Portal VertexPortalType;
  VertexPortalType vertices;


  const int xdim, ydim, zdim, cellsPerLayer, pointsPerLayer;
  const float isovalue, xmin, ymin, zmin, xmax, ymax, zmax;

  const int inputCellIdOffset;

  template<typename U, typename V, typename W, typename X>
  VTKM_CONT_EXPORT
  IsosurfaceFusedUniformGridFunctor(const float isovalue,
                               const int dims[3],
                               const U & field,
                               const U & source,
                               const W & vertices,
                               const X & scalars,
                               const V & outputVerticesLocHandle,
                               const int inputIdOffset=0):
  isovalue(isovalue),
  xdim(dims[0]), ydim(dims[1]), zdim(dims[2]),
  xmin(-1), ymin(-1), zmin(-1),
  xmax(1), ymax(1), zmax(1),
  field( field.PrepareForInput( DeviceAdapter() ) ),
  source( source.PrepareForInput( DeviceAdapter() ) ),
  outputVerticesLoc( outputVerticesLocHandle.PrepareForInput( DeviceAdapter() ) ),
  vertices(vertices),
  scalars(scalars),
  cellsPerLayer((xdim-1) * (ydim-1)),
  pointsPerLayer (xdim*ydim),
  inputCellIdOffset(inputIdOffset)
  {

  }

  VTKM_EXEC_EXPORT
  void operator()(vtkm::Id inputIndexId) const
  {
    // when operating on a slice of the data the inputIndexId
    // is relative to the start of the slice, so we need to
    // compute the proper global cell id.
    const int firstInputCellId = inputCellIdOffset + (inputIndexId * NumCellsToFuse);
    //you need to use the inputIndexId as the outputVerticesLoc array
    //is fused it self, and the size is based on the classify fusing
    vtkm::Id outputVertId = this->outputVerticesLoc.Get(inputIndexId);

    // Get data for this cell
    const int verticesForEdge[] = { 0, 1, 1, 2, 3, 2, 0, 3,
                                    4, 5, 5, 6, 7, 6, 4, 7,
                                    0, 4, 1, 5, 2, 6, 3, 7 };


    for(int i=0; i < NumCellsToFuse; i++)
      {
      const vtkm::Id inputCellId = firstInputCellId + i;

      const int x = inputCellId % (xdim - 1);
      const int y = (inputCellId / (xdim - 1)) % (ydim -1);
      const int z = inputCellId / cellsPerLayer;

      // Compute indices for the eight vertices of this cell
      const int i0 = x    + y*xdim + z * pointsPerLayer;
      const int i1 = i0   + 1;
      const int i2 = i0   + 1 + xdim;
      const int i3 = i0   + xdim;
      const int i4 = i0   + pointsPerLayer;
      const int i5 = i1   + pointsPerLayer;
      const int i6 = i2   + pointsPerLayer;
      const int i7 = i3   + pointsPerLayer;

      // Get the field values at these eight vertices
      float f[8];
      f[0] = this->field.Get(i0);
      f[1] = this->field.Get(i1);
      f[2] = this->field.Get(i2);
      f[3] = this->field.Get(i3);
      f[4] = this->field.Get(i4);
      f[5] = this->field.Get(i5);
      f[6] = this->field.Get(i6);
      f[7] = this->field.Get(i7);

      // Compute the Marching Cubes case number for this cell
      unsigned int cubeindex = 0;
      cubeindex += (f[0] > isovalue);
      cubeindex += (f[1] > isovalue)*2;
      cubeindex += (f[2] > isovalue)*4;
      cubeindex += (f[3] > isovalue)*8;
      cubeindex += (f[4] > isovalue)*16;
      cubeindex += (f[5] > isovalue)*32;
      cubeindex += (f[6] > isovalue)*64;
      cubeindex += (f[7] > isovalue)*128;

      const int numVertices  = numVerticesTable[cubeindex];
      // if(numVertices == 0)
      //   {
      //   continue;
      //   }
      // std::cout << "inputIndexId: " << inputIndexId << std::endl;
      // std::cout << "cellId: " << inputCellId << std::endl;
      // std::cout << "outputVertId: " << outputVertId << std::endl;
      // std::cout << "numVertices: " << numVertices << std::endl;
      // std::cout << std::endl;

      // Compute the coordinates of the uniform regular grid at each of the cell's eight vertices
      vtkm::Vec<FieldType, 3> p[8];
      p[0] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*x/(xdim-1)),     ymin+(ymax-ymin)*(1.0*y/(xdim-1)),     zmin+(zmax-zmin)*(1.0*z/(xdim-1)));
      p[1] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*(x+1)/(xdim-1)), ymin+(ymax-ymin)*(1.0*y/(xdim-1)),     zmin+(zmax-zmin)*(1.0*z/(xdim-1)));
      p[2] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*(x+1)/(xdim-1)), ymin+(ymax-ymin)*(1.0*(y+1)/(xdim-1)), zmin+(zmax-zmin)*(1.0*z/(xdim-1)));
      p[3] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*x/(xdim-1)),     ymin+(ymax-ymin)*(1.0*(y+1)/(xdim-1)), zmin+(zmax-zmin)*(1.0*z/(xdim-1)));
      p[4] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*x/(xdim-1)),     ymin+(ymax-ymin)*(1.0*y/(xdim-1)),     zmin+(zmax-zmin)*(1.0*(z+1)/(xdim-1)));
      p[5] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*(x+1)/(xdim-1)), ymin+(ymax-ymin)*(1.0*y/(xdim-1)),     zmin+(zmax-zmin)*(1.0*(z+1)/(xdim-1)));
      p[6] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*(x+1)/(xdim-1)), ymin+(ymax-ymin)*(1.0*(y+1)/(xdim-1)), zmin+(zmax-zmin)*(1.0*(z+1)/(xdim-1)));
      p[7] = vtkm::make_Vec(xmin+(xmax-xmin)*(1.0*x/(xdim-1)),     ymin+(ymax-ymin)*(1.0*(y+1)/(xdim-1)), zmin+(zmax-zmin)*(1.0*(z+1)/(xdim-1)));

      // Get the scalar source values at the eight vertices
      float s[8];
      s[0] = this->source.Get(i0);
      s[1] = this->source.Get(i1);
      s[2] = this->source.Get(i2);
      s[3] = this->source.Get(i3);
      s[4] = this->source.Get(i4);
      s[5] = this->source.Get(i5);
      s[6] = this->source.Get(i6);
      s[7] = this->source.Get(i7);

      // Interpolate for vertex positions and associated scalar values
      for (int v = 0; v < numVertices; v++)
        {
        const int edge = triTable[cubeindex*16 + v];
        const int v0   = verticesForEdge[2*edge];
        const int v1   = verticesForEdge[2*edge + 1];
        const float t  = (isovalue - f[v0]) / (f[v1] - f[v0]);

        this->vertices.Set(outputVertId + v, lerp(p[v0], p[v1], t));
        this->scalars.Set(outputVertId + v, lerp(s[v0], s[v1], t));
        }

      outputVertId += numVertices;
      }
  }
};

/// \brief Computes Marching Cubes case number for each cell, along with the number of triangles generated by that case
///
template <typename FieldType, typename CellNumType>
class ClassifyCellOutputTri : public vtkm::worklet::WorkletMapField
{
public:
  typedef void ControlSignature(FieldIn<IdType> inputCellId, FieldOut<AllTypes> numCellsOut);
  typedef _2 ExecutionSignature(_1);
  typedef _1 InputDomain;

  typedef typename PortalTypes<FieldType>::PortalConst FieldPortalType;
  FieldPortalType pointData;

  float isovalue;
  int xdim, ydim, zdim;
  int cellsPerLayer;
  int pointsPerLayer;

  template<typename T>
  VTKM_CONT_EXPORT
  ClassifyCellOutputTri(const T& pointHandle,
                        float iso,
                        int dims[3]) :
         pointData( pointHandle.PrepareForInput( DeviceAdapter() ) ),
         isovalue( iso ),
         xdim(dims[0]),
         ydim(dims[1]),
         zdim(dims[2]),
         cellsPerLayer((xdim - 1) * (ydim - 1)),
         pointsPerLayer (xdim*ydim)
   {

   }

  VTKM_EXEC_EXPORT
  CellNumType operator()(vtkm::Id cellId) const
  {
    // Compute 3D indices of this cell
    const int x = cellId % (xdim - 1);
    const int y = (cellId / (xdim - 1)) % (ydim -1);
    const int z = cellId / cellsPerLayer;

    // Compute indices for the eight vertices of this cell
    const int i0 = x    + y*xdim + z * pointsPerLayer;
    const int i1 = i0   + 1;
    const int i2 = i0   + 1 + xdim;
    const int i3 = i0   + xdim;
    const int i4 = i0   + pointsPerLayer;
    const int i5 = i1   + pointsPerLayer;
    const int i6 = i2   + pointsPerLayer;
    const int i7 = i3   + pointsPerLayer;

    // Get the field values at these eight vertices
    const float f0 = this->pointData.Get(i0);
    const float f1 = this->pointData.Get(i1);
    const float f2 = this->pointData.Get(i2);
    const float f3 = this->pointData.Get(i3);
    const float f4 = this->pointData.Get(i4);
    const float f5 = this->pointData.Get(i5);
    const float f6 = this->pointData.Get(i6);
    const float f7 = this->pointData.Get(i7);

    // Compute the Marching Cubes case number for this cell
    unsigned int cubeindex = (f0 > isovalue);
    cubeindex += (f1 > isovalue)*2;
    cubeindex += (f2 > isovalue)*4;
    cubeindex += (f3 > isovalue)*8;
    cubeindex += (f4 > isovalue)*16;
    cubeindex += (f5 > isovalue)*32;
    cubeindex += (f6 > isovalue)*64;
    cubeindex += (f7 > isovalue)*128;

    return CellNumType(numVerticesTable[cubeindex] / 3);
  }
};

/// \brief Compute isosurface vertices and scalars
///
template <typename FieldType, typename OutputType>
class IsosurfaceSingleTri : public vtkm::worklet::WorkletMapField
{
public:
  typedef void ControlSignature(FieldIn<IdType> inputCellId,
                                FieldIn<IdType> inputIteration);
  typedef void ExecutionSignature(WorkIndex, _1, _2);
  typedef _1 InputDomain;

  typedef typename PortalTypes< FieldType >::PortalConst FieldPortalType;
  FieldPortalType field, source;

  typedef typename PortalTypes< OutputType >::Portal ScalarPortalType;
  ScalarPortalType scalars;

  typedef typename PortalTypes< vtkm::Vec<vtkm::Float32,3> >::Portal VertexPortalType;
  VertexPortalType vertices;

  const int xdim, ydim, zdim, cellsPerLayer, pointsPerLayer;
  const float isovalue, xmin, ymin, zmin, xmax, ymax, zmax;

  const int inputCellIdOffset;

  template<typename U, typename W, typename X>
  VTKM_CONT_EXPORT
  IsosurfaceSingleTri( const float isovalue,
                       const int dims[3],
                       const U & field,
                       const U & source,
                       const W & vertices,
                       const X & scalars,
                       const int inputIdOffset=0):
  isovalue(isovalue),
  xdim(dims[0]), ydim(dims[1]), zdim(dims[2]),
  xmin(-1), ymin(-1), zmin(-1),
  xmax(1), ymax(1), zmax(1),
  field( field.PrepareForInput( DeviceAdapter() ) ),
  source( source.PrepareForInput( DeviceAdapter() ) ),
  vertices(vertices),
  scalars(scalars),
  cellsPerLayer((xdim-1) * (ydim-1)),
  pointsPerLayer (xdim*ydim),
  inputCellIdOffset(inputIdOffset)
  {

  }

  VTKM_EXEC_EXPORT
  void operator()(vtkm::Id outputCellId, vtkm::Id inputIndexId, vtkm::Id inputLowerBounds) const
  {
    // Get data for this cell
    const int verticesForEdge[] = { 0, 1, 1, 2, 3, 2, 0, 3,
                                    4, 5, 5, 6, 7, 6, 4, 7,
                                    0, 4, 1, 5, 2, 6, 3, 7 };

    // when operating on a slice of the data the inputIndexId
    // is relative to the start of the slice, so we need to
    // compute the proper global cell id.
    const int inputCellId = inputCellIdOffset + inputIndexId;

    const int x = inputCellId % (xdim - 1);
    const int y = (inputCellId / (xdim - 1)) % (ydim -1);
    const int z = inputCellId / cellsPerLayer;

    // Compute indices for the eight vertices of this cell
    const int i0 = x    + y*xdim + z * pointsPerLayer;
    const int i1 = i0   + 1;
    const int i2 = i0   + 1 + xdim;
    const int i3 = i0   + xdim;
    const int i4 = i0   + pointsPerLayer;
    const int i5 = i1   + pointsPerLayer;
    const int i6 = i2   + pointsPerLayer;
    const int i7 = i3   + pointsPerLayer;

    // Get the field values at these eight vertices
    float f[8];
    f[0] = this->field.Get(i0);
    f[1] = this->field.Get(i1);
    f[2] = this->field.Get(i2);
    f[3] = this->field.Get(i3);
    f[4] = this->field.Get(i4);
    f[5] = this->field.Get(i5);
    f[6] = this->field.Get(i6);
    f[7] = this->field.Get(i7);

    // Compute the Marching Cubes case number for this cell
    unsigned int cubeindex = 0;
    cubeindex += (f[0] > isovalue);
    cubeindex += (f[1] > isovalue)*2;
    cubeindex += (f[2] > isovalue)*4;
    cubeindex += (f[3] > isovalue)*8;
    cubeindex += (f[4] > isovalue)*16;
    cubeindex += (f[5] > isovalue)*32;
    cubeindex += (f[6] > isovalue)*64;
    cubeindex += (f[7] > isovalue)*128;

    // Compute the coordinates of the uniform regular grid at each of the cell's eight vertices
    vtkm::Vec<FieldType, 3> p[8];
    {
    //if we have offset and spacing, can we simplify this computation
    vtkm::Vec<FieldType, 3> offset = vtkm::make_Vec(xmin+(xmax-xmin),
                                                    ymin+(ymax-ymin),
                                                    zmin+(zmax-zmin) );

    vtkm::Vec<FieldType, 3> spacing = vtkm::make_Vec( 1.0 /(xdim-1),
                                                      1.0 /(ydim-1),
                                                      1.0 /(zdim-1));

    vtkm::Vec<FieldType, 3> firstPoint = offset * spacing *  vtkm::make_Vec( x, y, z );
    vtkm::Vec<FieldType, 3> secondPoint = offset * spacing * vtkm::make_Vec( x+1, y+1, z+1 );

    p[0] = vtkm::make_Vec( firstPoint[0],   firstPoint[1],   firstPoint[2]);
    p[1] = vtkm::make_Vec( secondPoint[0],  firstPoint[1],   firstPoint[2]);
    p[2] = vtkm::make_Vec( secondPoint[0],  secondPoint[1],  firstPoint[2]);
    p[3] = vtkm::make_Vec( firstPoint[0],   secondPoint[1],  firstPoint[2]);
    p[4] = vtkm::make_Vec( firstPoint[0],   firstPoint[1],   secondPoint[2]);
    p[5] = vtkm::make_Vec( secondPoint[0],  firstPoint[1],   secondPoint[2]);
    p[6] = vtkm::make_Vec( secondPoint[0],  secondPoint[1],  secondPoint[2]);
    p[7] = vtkm::make_Vec( firstPoint[0],   secondPoint[1],  secondPoint[2]);
    }

    // Get the scalar source values at the eight vertices
    float s[8];
    s[0] = this->source.Get(i0);
    s[1] = this->source.Get(i1);
    s[2] = this->source.Get(i2);
    s[3] = this->source.Get(i3);
    s[4] = this->source.Get(i4);
    s[5] = this->source.Get(i5);
    s[6] = this->source.Get(i6);
    s[7] = this->source.Get(i7);

    // Interpolate for vertex positions and associated scalar values
    const vtkm::Id inputIteration = (outputCellId - inputLowerBounds);
    const vtkm::Id outputVertId = outputCellId * 3;
    const vtkm::Id cellOffset = cubeindex*16 + (inputIteration * 3);
    for (int v = 0; v < 3; v++)
      {
      const int edge = triTable[cellOffset + v];
      const int v0   = verticesForEdge[2*edge];
      const int v1   = verticesForEdge[2*edge + 1];
      const float t  = (isovalue - f[v0]) / (f[v1] - f[v0]);

      this->vertices.Set(outputVertId + v, lerp(p[v0], p[v1], t));
      this->scalars.Set(outputVertId + v, lerp(s[v0], s[v1], t));
      }
  }
};

}