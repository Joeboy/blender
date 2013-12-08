/*
 * Copyright 2013, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef PTC_CLOTH_H
#define PTC_CLOTH_H

//#include <Alembic/AbcGeom/IPoints.h>
//#include <Alembic/AbcGeom/OPoints.h>

#include "reader.h"
#include "schema.h"
#include "writer.h"

struct Object;
struct ClothModifierData;

namespace PTC {

class ClothWriter : public Writer {
public:
	ClothWriter(Scene *scene, Object *ob, ClothModifierData *clmd);
	~ClothWriter();
	
	void write_sample();
	
private:
	Object *m_ob;
	ClothModifierData *m_clmd;
	
//	AbcGeom::OPoints m_points;
};

class ClothReader : public Reader {
public:
	ClothReader(Scene *scene, Object *ob, ClothModifierData *clmd);
	~ClothReader();
	
	PTCReadSampleResult read_sample(float frame);
	
private:
	Object *m_ob;
	ClothModifierData *m_clmd;
	
//	AbcGeom::IPoints m_points;
};

} /* namespace PTC */

#endif  /* PTC_CLOTH_H */
