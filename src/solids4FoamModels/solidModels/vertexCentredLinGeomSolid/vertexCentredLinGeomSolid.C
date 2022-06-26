/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright held by original author
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the
    Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM; if not, write to the Free Software Foundation,
    Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

\*---------------------------------------------------------------------------*/

#include "vertexCentredLinGeomSolid.H"
#include "addToRunTimeSelectionTable.H"
#include "sparseMatrix.H"
#include "symmTensor4thOrder.H"
#include "vfvcCellPoint.H"
#include "vfvmCellPoint.H"
#include "fvcDiv.H"
// #include "uniformFixedValuePointPatchFields.H"
#include "fixedValuePointPatchFields.H"
#include "solidTractionPointPatchVectorField.H"
#include "sparseMatrixTools.H"
#include "BlockSolverPerformance.H"
#include "symmetryPointPatchFields.H"
#include "fixedDisplacementZeroShearPointPatchVectorField.H"
#include <petscksp.h>

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace Foam
{

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

namespace solidModels
{

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

defineTypeNameAndDebug(vertexCentredLinGeomSolid, 0);
addToRunTimeSelectionTable(solidModel, vertexCentredLinGeomSolid, dictionary);


// * * * * * * * * * * *  Private Member Functions * * * * * * * * * * * * * //

void vertexCentredLinGeomSolid::updateSource
(
    vectorField& source,
    const labelList& dualCellToPoint
)
{
    if (debug)
    {
        Info<< "void vertexCentredLinGeomSolid::updateSource(...): start"
            << endl;
    }

    // Reset to zero
    source = vector::zero;

    // The source vector is -F, where:
    // F = div(sigma) + rho*g - rho*d2dt2(D)

    // Point volume field
    const scalarField& pointVolI = pointVol_.internalField();

    // Point density field
    const scalarField& pointRhoI = pointRho_.internalField();

    // Calculate the tractions on the dual faces
    surfaceVectorField dualTraction =
        ((dualMesh().Sf()/dualMesh().magSf()) & dualSigmaf_);

    // Enforce extract tractions on traction boundaries
    enforceTractionBoundaries
    (
        pointD(), dualTraction, mesh(), dualMeshMap().pointToDualFaces()
    );

    // Set coupled boundary (e.g. processor) traction fields to zero: this
    // ensures their global contribution is zero
    forAll(dualTraction.boundaryField(), patchI)
    {
        if (dualTraction.boundaryField()[patchI].coupled())
        {
            dualTraction.boundaryField()[patchI] = vector::zero;
        }
    }

    // Calculate divergence of stress for the dual cells
    // const vectorField dualDivSigma = fvc::div(dualMesh().Sf() & dualSigmaf_);
    const vectorField dualDivSigma = fvc::div(dualTraction*dualMesh().magSf());

    // Map dual cell field to primary mesh point field
    vectorField pointDivSigma(mesh().nPoints(), vector::zero);
    forAll(dualDivSigma, dualCellI)
    {
        const label pointID = dualCellToPoint[dualCellI];
        pointDivSigma[pointID] = dualDivSigma[dualCellI];
    }

    // Add surface forces to source
    source -= pointDivSigma*pointVolI;

    // Add gravity body forces
    source -= pointRhoI*g().value()*pointVolI;

    // Add transient term
    source += vfvc::d2dt2
    (
        mesh().schemesDict(),
        pointD(),
        pointU_,
        pointA_,
        pointRho_,
        pointVol_,
        debug
    );

    if (debug)
    {
        Info<< "void vertexCentredLinGeomSolid::updateSource(...): end"
            << endl;
    }
}


void vertexCentredLinGeomSolid::setFixedDofs
(
    const pointVectorField& pointD,
    boolList& fixedDofs,
    pointField& fixedDofValues,
    symmTensorField& fixedDofDirections
) const
{
    // Flag all fixed DOFs

    forAll(pointD.boundaryField(), patchI)
    {
        if
        (
            // isA<uniformFixedValuePointPatchVectorField>
            isA<fixedValuePointPatchVectorField>
            (
                pointD.boundaryField()[patchI]
            )
        )
        {
            // const uniformFixedValuePointPatchVectorField& dispPatch =
            //     refCast<const uniformFixedValuePointPatchVectorField>
            // const fixedValuePointPatchVectorField& dispPatch =
            //     refCast<const fixedValuePointPatchVectorField>
            //     (
            //         pointD.boundaryField()[patchI]
            //     );

            // const vector& disp = dispPatch.uniformValue();

            const labelList& meshPoints =
                pointD.mesh().mesh().boundaryMesh()[patchI].meshPoints();

            forAll(meshPoints, pI)
            {
                const label pointID = meshPoints[pI];
                const vector& disp = pointD[pointID];

                // Check if this point has already been fixed
                if (fixedDofs[pointID])
                {
                    // Check if the existing prescribed displacement is
                    // consistent with the new one
                    if
                    (
                        mag
                        (
                            fixedDofDirections[pointID]
                          & (fixedDofValues[pointID] - disp)
                        ) > SMALL
                    )
                    {
                        FatalErrorIn
                        (
                            "void vertexCentredLinGeomSolid::setFixedDofs(...)"
                        )   << "Inconsistent displacements prescribed at point "
                            << "= " << pointD.mesh().mesh().points()[pointID]
                            << abort(FatalError);
                    }

                    // Set all directions as fixed, just in case it was
                    // previously marked as a symmetry point
                    fixedDofDirections[pointID] = symmTensor(I);
                }
                else
                {
                    fixedDofs[pointID] = true;
                    fixedDofValues[pointID] = disp;
                    fixedDofDirections[pointID] = symmTensor(I);
                }
            }
        }
        else if
        (
            isA<symmetryPointPatchVectorField>
            (
                pointD.boundaryField()[patchI]
            )
         || isA<fixedDisplacementZeroShearPointPatchVectorField>
            (
                pointD.boundaryField()[patchI]
            )
        )
        {
            const labelList& meshPoints =
                pointD.mesh().boundary()[patchI].meshPoints();
            const vectorField& pointNormals =
                pointD.mesh().boundary()[patchI].pointNormals();

            scalarField normalDisp(meshPoints.size(), 0.0);
            if
            (
                isA<fixedDisplacementZeroShearPointPatchVectorField>
                (
                    pointD.boundaryField()[patchI]
                )
            )
            {
                normalDisp =
                (
                    pointNormals
                  & pointD.boundaryField()[patchI].patchInternalField()
                );

                if (debug)
                {
                    Info<< "normalDisp = " << normalDisp << endl;
                }
            }

            forAll(meshPoints, pI)
            {
                const label pointID = meshPoints[pI];

                // Check if this point has already been fixed
                if (fixedDofs[pointID])
                {
                    // Check if the existing prescribed displacement is
                    // consistent with the current condition
                    if
                    (
                        mag
                        (
                            (pointNormals[pI] & fixedDofValues[pointID])
                          - normalDisp[pI]
                        ) > SMALL
                    )
                    {
                        FatalErrorIn
                        (
                            "void vertexCentredLinGeomSolid::setFixedDofs(...)"
                        )   << "Inconsistent displacements prescribed at point "
                            << "= " << pointD.mesh().mesh().points()[pointID]
                            << abort(FatalError);
                    }

                    // If the point is not fully fuxed then make sure the normal
                    // direction is fixed
                    if (mag(fixedDofDirections[pointID] - symmTensor(I)) > 0)
                    {
                        // If the directions are orthogonal we can add them
                        const symmTensor curDir = sqr(pointNormals[pI]);
                        if (mag(fixedDofDirections[pointID] & curDir) > 0)
                        {
                            FatalError
                                << "Point " << pointID << " is fixed in two "
                                << "directions: this is only implemented for "
                                << "Cartesian axis directions" << abort(FatalError);
                        }

                        fixedDofDirections[pointID] += curDir;
                    }
                }
                else
                {
                    fixedDofs[pointID] = true;
                    fixedDofValues[pointID] = normalDisp[pI]*pointNormals[pI];
                    fixedDofDirections[pointID] = sqr(pointNormals[pI]);
                }
            }
        }
    }
}


void vertexCentredLinGeomSolid::enforceTractionBoundaries
(
    const pointVectorField& pointD,
    surfaceVectorField& dualTraction,
    const fvMesh& mesh,
    const labelListList& pointToDualFaces
) const
{
    const pointMesh& pMesh = pointD.mesh();
    const fvMesh& dualMesh = dualTraction.mesh();

    forAll(pointD.boundaryField(), patchI)
    {
        if
        (
            isA<solidTractionPointPatchVectorField>
            (
                pointD.boundaryField()[patchI]
            )
        )
        {
            const solidTractionPointPatchVectorField& tracPatch =
                refCast<const solidTractionPointPatchVectorField>
                (
                    pointD.boundaryField()[patchI]
                );

            const labelList& meshPoints =
                mesh.boundaryMesh()[patchI].meshPoints();

            // Primary mesh point normals
            const vectorField& n =
                pMesh.boundary()[patchI].pointNormals();

            // Primary mesh point tractions
            const vectorField totalTraction =
                tracPatch.traction() - n*tracPatch.pressure();

            // Create dual mesh faces traction field
            vectorField dualFaceTraction
            (
                dualMesh.boundaryMesh()[patchI].size(), vector::zero
            );

            // Multiple points map to each dual face so we will count them
            // and then divide the dualFaceTraction by this field so that it is
            // the average of all the points that map to it
            scalarField nPointsPerDualFace(dualFaceTraction.size(), 0.0);

            // Map from primary mesh point field to second mesh face field using
            // the pointToDualFaces map
            forAll(totalTraction, pI)
            {
                const label pointID = meshPoints[pI];
                const labelList& curDualFaces = pointToDualFaces[pointID];

                forAll(curDualFaces, dfI)
                {
                    const label dualFaceID = curDualFaces[dfI];

                    if (!dualMesh.isInternalFace(dualFaceID))
                    {
                        // Check which patch this dual face belongs to
                        const label dualPatchID =
                            dualMesh.boundaryMesh().whichPatch(dualFaceID);

                        if (dualPatchID == patchI)
                        {
                            // Find local face index
                            const label localDualFaceID =
                                dualFaceID
                              - dualMesh.boundaryMesh()[dualPatchID].start();

                            // Set dual face traction
                            dualFaceTraction[localDualFaceID] +=
                                totalTraction[pI];

                            // Update the count for this face
                            nPointsPerDualFace[localDualFaceID]++;
                        }
                    }
                }
            }

            if (gMin(nPointsPerDualFace) < 1)
            {
                FatalErrorIn
                (
                    "void vertexCentredLinGeomSolid::"
                    "enforceTractionBoundaries(...)"
                )   << "Problem setting tractions: gMin(nPointsPerDualFace) < 1"
                    << nl << "nPointsPerDualFace = " << nPointsPerDualFace
                    << abort(FatalError);
            }

            // Take the average
            dualFaceTraction /= nPointsPerDualFace;

            // Overwrite the dual patch face traction
            dualTraction.boundaryField()[patchI] = dualFaceTraction;
        }
        else if
        (
            isA<symmetryPointPatchVectorField>(pointD.boundaryField()[patchI])
         || isA<fixedDisplacementZeroShearPointPatchVectorField>
            (
                pointD.boundaryField()[patchI]
            )
        )
        {
            // Set the dual patch face shear traction to zero
            const vectorField n = dualMesh.boundary()[patchI].nf();
            dualTraction.boundaryField()[patchI] =
                (sqr(n) & dualTraction.boundaryField()[patchI]);
        }
    }
}

bool vertexCentredLinGeomSolid::vertexCentredLinGeomSolid::converged
(
    const label iCorr,
    scalar& initResidual,
    const scalar res,
    const label nInterations,
    const pointVectorField& pointD,
    const vectorField& pointDcorr
) const
{
    // Calculate the residual as the root mean square of the correction
    const scalar residualAbs = gSum(magSqr(pointDcorr));

    // Store initial residual
    if (iCorr == 0)
    {
        initResidual = residualAbs;

        // If the initial residual is small then convergence has been achieved
        if (initResidual < SMALL)
        {
            Info<< "    Initial residual is less than 1e-15"
                << "    Converged" << endl;
            return true;
        }
        Info<< "    Initial residual = " << initResidual << endl;
    }

    // Define a normalised residual wrt the initial residual
    const scalar residualNorm = residualAbs/initResidual;

    // Calculate the maximum displacement
    const scalar maxMagD = gMax(mag(pointD.internalField()));

    // Print information
    Info<< "    Iter = " << iCorr
        << ", relRef = " << residualNorm
        << ", res = " << res
        << ", resAbs = " << residualAbs
        << ", nIters = " << nInterations
        << ", maxD = " << maxMagD << endl;

    // Check for convergence
    if (residualNorm < solutionTol())
    {
        Info<< "    Converged" << endl;
        return true;
    }
    else if (iCorr >= nCorr() - 1)
    {
        Warning
            << "Max iterations reached within the momentum Newton-Raphson loop"
            << endl;
        return true;
    }

    // Convergence has not been reached
    return false;
}


scalar vertexCentredLinGeomSolid::calculateLineSearchSlope
(
    const scalar eta, const vectorField& pointDcorr, const scalar zeta
)
{
    // Store pointD as we will reset it after changing it
    pointD().storePrevIter();

    // Update pointD
    pointD().internalField() += eta*pointDcorr;
    pointD().correctBoundaryConditions();

    // Calculate gradD at dual faces
    dualGradDf_ = vfvc::fGrad
    (
        pointD(),
        mesh(),
        dualMesh(),
        dualMeshMap().dualFaceToCell(),
        dualMeshMap().dualCellToPoint(),
        zeta
    );

    // Calculate stress at dual faces
    dualMechanicalPtr_().correct(dualSigmaf_);

    // Update the source vector
    vectorField source(mesh().nPoints(), vector::zero);
    pointD().correctBoundaryConditions();
    updateSource(source, dualMeshMap().dualCellToPoint());

    // Reset pointD
    pointD() = pointD().prevIter();

    // Return the slope
    return gSum(pointDcorr & source);
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

vertexCentredLinGeomSolid::vertexCentredLinGeomSolid
(
    Time& runTime,
    const word& region
)
:
    solidModel(typeName, runTime, region),
    dualMechanicalPtr_
    (
        new dualMechanicalModel
        (
            dualMesh(),
            nonLinGeom(),
            incremental(),
            mechanical(),
            dualMeshMap().dualFaceToCell()
        )
    ),
    fullNewton_(solidModelDict().lookup("fullNewton")),
    steadyState_(false),
    twoD_(sparseMatrixTools::checkTwoD(mesh())),
    fixedDofs_(mesh().nPoints(), false),
    fixedDofValues_(fixedDofs_.size(), vector::zero),
    fixedDofDirections_(fixedDofs_.size(), symmTensor::zero),
    fixedDofScale_
    (
        solidModelDict().lookupOrDefault<scalar>
        (
            "fixedDofScale",
            (
                average(mechanical().impK())
               *Foam::sqrt(gAverage(mesh().magSf()))
            ).value()
        )
    ),
    pointU_
    (
        IOobject
        (
            "pointU",
            runTime.timeName(),
            mesh(),
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        pMesh(),
        dimensionedVector("0", dimVelocity, vector::zero)
    ),
    pointA_
    (
        IOobject
        (
            "pointA",
            runTime.timeName(),
            mesh(),
            IOobject::READ_IF_PRESENT,
            IOobject::NO_WRITE
        ),
        pMesh(),
        dimensionedVector("0", dimVelocity/dimTime, vector::zero)
    ),
    pointRho_
    (
        IOobject
        (
            "point(rho)",
            runTime.timeName(),
            mesh(),
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        pMesh(),
        dimensionedScalar("0", dimDensity, 0.0)
    ),
    pointVol_
    (
        IOobject
        (
            "pointVolumes",
            runTime.timeName(),
            mesh(),
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        pMesh(),
        dimensionedScalar("0", dimVolume, 0.0)
    ),
    dualGradDf_
    (
        IOobject
        (
            "grad(D)f",
            runTime.timeName(),
            dualMesh(),
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        dualMesh(),
        dimensionedTensor("zero", dimless, tensor::zero),
        "calculated"
    ),
    dualSigmaf_
    (
        IOobject
        (
            "sigmaf",
            runTime.timeName(),
            dualMesh(),
            IOobject::NO_READ,
            IOobject::NO_WRITE
        ),
        dualMesh(),
        dimensionedSymmTensor("zero", dimPressure, symmTensor::zero),
        "calculated"
    ),
    globalPointIndices_(mesh())
{
    // Create dual mesh and set write option
    dualMesh().writeOpt() = IOobject::NO_WRITE;

    // pointD field must be defined
    pointDisRequired();

    // Set fixed degree of freedom list
    setFixedDofs(pointD(), fixedDofs_, fixedDofValues_, fixedDofDirections_);

    // Set point density field
    mechanical().volToPoint().interpolate(rho(), pointRho_);

    // Set the pointVol field
    // Map dualMesh cell volumes to the primary mesh points
    scalarField& pointVolI = pointVol_.internalField();
    const scalarField& dualCellVol = dualMesh().V();
    const labelList& dualCellToPoint = dualMeshMap().dualCellToPoint();
    forAll(dualCellToPoint, dualCellI)
    {
        // Find point which maps to this dual cell
        const label pointID = dualCellToPoint[dualCellI];

        // Map the cell volume
        pointVolI[pointID] = dualCellVol[dualCellI];
    }

    // Store old time fields
    pointD().oldTime().storeOldTime();
    pointU_.oldTime().storeOldTime();
    pointA_.storeOldTime();

    // Write fixed degree of freedom equation scale
    Info<< "fixedDofScale: " << fixedDofScale_ << endl;

    // Disable the writing of the unused fields
    D().writeOpt() = IOobject::NO_WRITE;
    D().oldTime().oldTime().writeOpt() = IOobject::NO_WRITE;
    DD().writeOpt() = IOobject::NO_WRITE;
    DD().oldTime().oldTime().writeOpt() = IOobject::NO_WRITE;
    U().writeOpt() = IOobject::NO_WRITE;
    pointDD().writeOpt() = IOobject::NO_WRITE;
}


// * * * * * * * * * * * * * * * *  Destructors  * * * * * * * * * * * * * * //

vertexCentredLinGeomSolid::~vertexCentredLinGeomSolid()
{
    // if (Switch(solidModelDict().lookup("usePETSc")))
    // {
    //     PetscFinalize();
    // }
}

// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

bool vertexCentredLinGeomSolid::evolve()
{
    Info<< "Evolving solid solver" << endl;

    // Initialise matrix
    sparseMatrix matrix(sum(globalPointIndices_.stencilSize()));

    // Store material tangent field for dual mesh faces
    Field<symmTensor4thOrder> materialTangent =
        dualMechanicalPtr_().materialTangentFaceField();

    // Lookup compact edge gradient factor
    const scalar zeta(solidModelDict().lookupOrDefault<scalar>("zeta", 0.2));
    if (debug)
    {
        Info<< "zeta: " << zeta << endl;
    }

    // Global point index lists
    const boolList& ownedByThisProc = globalPointIndices_.ownedByThisProc();
    const labelList& localToGlobalPointMap =
        globalPointIndices_.localToGlobalPointMap();

    if (!fullNewton_)
    {
        // Assemble matrix once per time-step
        Info<< "    Assembling the matrix" << endl;

        // Add div(sigma) coefficients
        vfvm::divSigma
        (
            matrix,
            mesh(),
            dualMesh(),
            dualMeshMap().dualFaceToCell(),
            dualMeshMap().dualCellToPoint(),
            materialTangent,
            fixedDofs_,
            fixedDofDirections_,
            fixedDofScale_,
            zeta,
            debug
        );

        // Add d2dt2 coefficients
        vfvm::d2dt2
        (
            mesh().schemesDict(),
            runTime().deltaTValue(),
            pointD().name(),
            matrix,
            pointRho_.internalField(),
            pointVol_.internalField(),
            debug
        );
    }

    // Solution field: point displacement correction
    vectorField pointDcorr(pointD().internalField().size(), vector::zero);

    // Newton-Raphson loop over momentum equation
    int iCorr = 0;
    scalar initResidual = 0.0;
    BlockSolverPerformance<vector> solverPerf;
    do
    {
        // Calculate gradD at dual faces
        dualGradDf_ = vfvc::fGrad
        (
            pointD(),
            mesh(),
            dualMesh(),
            dualMeshMap().dualFaceToCell(),
            dualMeshMap().dualCellToPoint(),
            zeta,
            debug
        );

        // Calculate stress at dual faces
        dualMechanicalPtr_().correct(dualSigmaf_);

        // Update the source vector
        vectorField source(mesh().nPoints(), vector::zero);
        pointD().correctBoundaryConditions();
        updateSource(source, dualMeshMap().dualCellToPoint());

        if (fullNewton_)
        {
            // Info<< "    Assembling the matrix" << endl;

            // Assemble the matrix once per outer iteration
            matrix.clear();

            // Update material tangent
            materialTangent = dualMechanicalPtr_().materialTangentFaceField();

            // Add div(sigma) coefficients
            vfvm::divSigma
            (
                matrix,
                mesh(),
                dualMesh(),
                dualMeshMap().dualFaceToCell(),
                dualMeshMap().dualCellToPoint(),
                materialTangent,
                fixedDofs_,
                fixedDofDirections_,
                fixedDofScale_,
                zeta
            );

            // Add d2dt2 coefficients
            vfvm::d2dt2
            (
                mesh().schemesDict(),
                runTime().deltaTValue(),
                pointD().name(),
                matrix,
                pointRho_.internalField(),
                pointVol_.internalField(),
                debug
            );
        }

        // Enforce fixed DOF on the linear system
        sparseMatrixTools::enforceFixedDof
        (
            matrix,
            source,
            fixedDofs_,
            fixedDofDirections_,
            fixedDofValues_,
            fixedDofScale_
        );

        // Solve linear system for displacement correction
        if (debug)
        {
            Info<< "bool vertexCentredLinGeomSolid::evolve(): "
                << " solving linear system: start" << endl;
        }
        else
        {
            Info<< "    Solving" << endl;
        }

        if (Switch(solidModelDict().lookup("usePETSc")))
        {
            fileName optionsFile(solidModelDict().lookup("optionsFile"));
            solverPerf = sparseMatrixTools::solveLinearSystemPETSc
            (
                matrix,
                source,
                pointDcorr,
                twoD_,
                optionsFile,
                mesh().points(),
                ownedByThisProc,
                localToGlobalPointMap,
                globalPointIndices_.stencilSizeOwned(),
                globalPointIndices_.stencilSizeNotOwned(),
                debug
            );
        }
        else
        {
            // Use Eigen SparseLU direct solver
            sparseMatrixTools::solveLinearSystemEigen
            (
                matrix, source, pointDcorr, twoD_, false, debug
            );
        }

        if (debug)
        {
            Info<< "bool vertexCentredLinGeomSolid::evolve(): "
                << " solving linear system: end" << endl;
        }

        // Update point displacement field
        if (Switch(solidModelDict().lookup("lineSearch")))
        {
            // Set target tolerance for slope reduction
            const scalar rTol
            (
                solidModelDict().lookupOrDefault<scalar>("lineSearchRTol", 0.8)
            );
            const int maxIter
            (
                solidModelDict().lookupOrDefault<scalar>
                (
                    "lineSearchMaxIter", 10
                )
            );

            // Calculate initial slope
            const scalar s0 = gSum(pointDcorr & source);
            //Info<< "s0 = " << s0 << endl;

            // Set initial line search parameter
            scalar eta = 1.0;
            int lineSearchIter = 0;

            // Perform backtracking to find suitable eta
            do
            {
                lineSearchIter++;

                // Calculate slope at eta
                const scalar s1 = calculateLineSearchSlope
                (
                    eta, pointDcorr, zeta
                );

                // Calculate ratio of s1 to s0
                const scalar r = s1/s0;

                if (mag(r) < rTol)
                {
                    break;
                }
                else
                {
                    // Interpolate/extrapolate to find new eta
                    // Limit it to be less than 10
                    //eta = min(-1/(r - 1), 10);

                    if (r < 0)
                    {
                        // Simple back tracking
                        eta *= 0.5;
                    }
                    else
                    {
                        // Extrapolate
                        eta = min(-1/(r - 1), 10);
                    }
                }

                if (lineSearchIter == maxIter)
                {
                    Warning
                        << "Max line search iterations reached!" << endl;
                }
            }
            while (lineSearchIter < maxIter);

            // Update pointD and re-calculate source, then calculate s
            if (mag(eta - 1) > SMALL)
            {
                Info<< "        line search parameter = " << eta
                    << ", iter = " << lineSearchIter << endl;
            }
            pointD().internalField() += eta*pointDcorr;
        }
        else if (mesh().solutionDict().relaxField(pointD().name()))
        {
            // Relaxing the correction can help convergence
            // This is like a simple line search
            // Of course, an actual line search would be better: to-do!
            const scalar rf
            (
                mesh().solutionDict().fieldRelaxationFactor(pointD().name())
            );

            pointD().internalField() += rf*pointDcorr;
        }
        else
        {
            pointD().internalField() += pointDcorr;
        }
        pointD().correctBoundaryConditions();

        // Update point accelerations
        // Note: for NewmarkBeta, this needs to come before the pointU update
        pointA_.internalField() = vfvc::ddt(mesh().schemesDict(), pointU_);

        // Update point velocities
        pointU_.internalField() = vfvc::ddt(mesh().schemesDict(), pointD());
    }
    while
    (
        !converged
        (
            iCorr,
            initResidual,
            solverPerf.finalResidual()[vector::X],
            solverPerf.nIterations(),
            pointD(),
            pointDcorr
        ) && ++iCorr
    );

    // Calculate gradD at dual faces
    dualGradDf_ = vfvc::fGrad
    (
        pointD(),
        mesh(),
        dualMesh(),
        dualMeshMap().dualFaceToCell(),
        dualMeshMap().dualCellToPoint(),
        zeta,
        debug
    );

    // Update the increment of displacement
    pointDD() = pointD() - pointD().oldTime();

    // Calculate cell gradient
    // This assumes a constant gradient within each primary mesh cell
    gradD() = vfvc::grad(pointD(), mesh());

    // Map primary cell gradD field to sub-meshes for multi-material cases
    if (mechanical().PtrList<mechanicalLaw>::size() > 1)
    {
        mechanical().mapGradToSubMeshes(gradD());
    }

    // Update dual face stress field
    dualMechanicalPtr_().correct(dualSigmaf_);

    // Update primary mesh cell stress field, assuming it is constant per
    // primary mesh cell
    mechanical().correct(sigma());

    return true;
}


void vertexCentredLinGeomSolid::setTraction
(
    const label interfaceI,
    const label patchID,
    const vectorField& faceZoneTraction
)
{
    // Get point field on patch
    const vectorField traction
    (
        globalPatches()[interfaceI].interpolator().faceToPointInterpolate
        (
            globalPatches()[interfaceI].globalFaceToPatch(faceZoneTraction)
        )
    );

    // Lookup point patch field
    pointPatchVectorField& ptPatch = pointD().boundaryField()[patchID];

    if (ptPatch.type() == solidTractionPointPatchVectorField::typeName)
    {
        solidTractionPointPatchVectorField& patchD =
            refCast<solidTractionPointPatchVectorField>(ptPatch);

        patchD.traction() = traction;
    }
    else
    {
        FatalErrorIn
        (
            "void Foam::vertexCentredLinGeomSolid::setTraction\n"
            "(\n"
            "    fvPatchVectorField& tractionPatch,\n"
            "    const vectorField& traction\n"
            ")"
        )   << "Boundary condition "
            << ptPatch.type()
            << " for point patch " << ptPatch.patch().name()
            << " should instead be type "
            << solidTractionPointPatchVectorField::typeName
            << abort(FatalError);
    }
}


void vertexCentredLinGeomSolid::setPressure
(
    const label interfaceI,
    const label patchID,
    const scalarField& faceZonePressure
)
{
    // Get patch field
    const scalarField pressure
    (
        globalPatches()[interfaceI].interpolator().faceToPointInterpolate
        (
            globalPatches()[interfaceI].globalFaceToPatch(faceZonePressure)
        )
    );

    // Lookup point patch field
    pointPatchVectorField& ptPatch = pointD().boundaryField()[patchID];

    if (ptPatch.type() == solidTractionPointPatchVectorField::typeName)
    {
        solidTractionPointPatchVectorField& patchD =
            refCast<solidTractionPointPatchVectorField>(ptPatch);

        patchD.pressure() = pressure;
    }
    else
    {
        FatalErrorIn
        (
            "void Foam::vertexCentredLinGeomSolid::setPressure\n"
            "(\n"
            "    fvPatchVectorField& pressurePatch,\n"
            "    const vectorField& pressure\n"
            ")"
        )   << "Boundary condition "
            << ptPatch.type()
            << "for patch" << ptPatch.patch().name()
            << " should instead be type "
            << solidTractionPointPatchVectorField::typeName
            << abort(FatalError);
    }
}


// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace solidModels

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

} // End namespace Foam

// ************************************************************************* //