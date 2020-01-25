/*
 * Copyright 2015, Blender Foundation.
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
 *
 * Contributor(s):
 *    Joe Button
 */

#include "COM_RougeOneOperation.h"

RougeOneOperation::RougeOneOperation() : NodeOperation()
{
  this->addInputSocket(COM_DT_COLOR);
  this->addOutputSocket(COM_DT_COLOR);
  this->m_inputProgram = NULL;
}

void RougeOneOperation::initExecution()
{
  this->m_inputProgram = this->getInputSocketReader(0);
}

void RougeOneOperation::executePixelSampled(float output[4],
                                            float x,
                                            float y,
                                            PixelSampler sampler)
{
  float input[4];
  this->m_inputProgram->readSampled(input, x, y, sampler);
  output[0] = 1.0f;
  output[1] = input[1];
  output[2] = input[2];
  output[3] = input[3];
}

void RougeOneOperation::deinitExecution()
{
  this->m_inputProgram = NULL;
}