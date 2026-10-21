//! Scènes en triangles et structures d'accélération matérielles (ray query Vulkan via wgpu).
use crate::gpu::Gpu;
use crate::math::{v3, V3};
use crate::scene::{Aabb, Light};

/// Maillage indexé, un albédo par maillage.
#[derive(Clone)]
pub struct Mesh {
    pub pos: Vec<[f32; 3]>,
    pub idx: Vec<u32>,
    pub albedo: [f32; 3],
}

impl Mesh {
    fn new(albedo: [f32; 3]) -> Mesh {
        Mesh { pos: Vec::new(), idx: Vec::new(), albedo }
    }
    /// Quadrilatère a, b, c, d (dans l'ordre du contour).
    fn quad(&mut self, a: V3, b: V3, c: V3, d: V3) {
        let i = self.pos.len() as u32;
        for p in [a, b, c, d] {
            self.pos.push([p.x as f32, p.y as f32, p.z as f32]);
        }
        self.idx.extend_from_slice(&[i, i + 1, i + 2, i, i + 2, i + 3]);
    }
    /// Boîte alignée (6 faces).
    pub fn cuboid(albedo: [f32; 3], b: &Aabb) -> Mesh {
        let mut m = Mesh::new(albedo);
        let (l, h) = (b.min, b.max);
        let p = |x: f64, y: f64, z: f64| v3(x, y, z);
        m.quad(p(l.x, l.y, l.z), p(h.x, l.y, l.z), p(h.x, l.y, h.z), p(l.x, l.y, h.z));
        m.quad(p(l.x, h.y, l.z), p(l.x, h.y, h.z), p(h.x, h.y, h.z), p(h.x, h.y, l.z));
        m.quad(p(l.x, l.y, l.z), p(l.x, h.y, l.z), p(h.x, h.y, l.z), p(h.x, l.y, l.z));
        m.quad(p(l.x, l.y, h.z), p(h.x, l.y, h.z), p(h.x, h.y, h.z), p(l.x, h.y, h.z));
        m.quad(p(l.x, l.y, l.z), p(l.x, l.y, h.z), p(l.x, h.y, h.z), p(l.x, h.y, l.z));
        m.quad(p(h.x, l.y, l.z), p(h.x, h.y, l.z), p(h.x, h.y, h.z), p(h.x, l.y, h.z));
        m
    }
}

/// Instance : maillage + translation (les objets mobiles sont des instances déplacées).
#[derive(Clone, Copy)]
pub struct Instance {
    pub mesh: usize,
    pub offset: V3,
}

pub struct TriScene {
    pub meshes: Vec<Mesh>,
    pub instances: Vec<Instance>,
    pub lights: Vec<Light>,
    pub bounds: Aabb,
}

impl TriScene {
    pub fn instance_aabb(&self, i: usize) -> Aabb {
        let inst = self.instances[i];
        let m = &self.meshes[inst.mesh];
        let mut b = Aabb { min: V3::splat(f64::INFINITY), max: V3::splat(f64::NEG_INFINITY) };
        for p in &m.pos {
            let q = v3(p[0] as f64, p[1] as f64, p[2] as f64) + inst.offset;
            b = Aabb { min: b.min.min(q), max: b.max.max(q) };
        }
        b
    }
    pub fn triangles(&self) -> usize {
        self.instances.iter().map(|i| self.meshes[i.mesh].idx.len() / 3).sum()
    }
}

/// Cornell box (5,5 m), boîte haute fixe, boîte basse mobile (instance 1), plafonnier.
pub fn cornell(step: u32, light_on: bool) -> TriScene {
    let white = [0.73, 0.73, 0.73];
    let red = [0.65, 0.05, 0.05];
    let green = [0.12, 0.45, 0.15];
    let s = 5.5;
    let mut room = Mesh::new(white);
    let p = |x: f64, y: f64, z: f64| v3(x, y, z);
    room.quad(p(0.0, 0.0, 0.0), p(0.0, 0.0, s), p(s, 0.0, s), p(s, 0.0, 0.0)); // sol
    room.quad(p(0.0, s, 0.0), p(s, s, 0.0), p(s, s, s), p(0.0, s, s)); // plafond
    room.quad(p(0.0, 0.0, 0.0), p(s, 0.0, 0.0), p(s, s, 0.0), p(0.0, s, 0.0)); // fond
    let mut left = Mesh::new(red);
    left.quad(p(0.0, 0.0, 0.0), p(0.0, s, 0.0), p(0.0, s, s), p(0.0, 0.0, s));
    let mut right = Mesh::new(green);
    right.quad(p(s, 0.0, 0.0), p(s, 0.0, s), p(s, s, s), p(s, s, 0.0));
    let tall = Mesh::cuboid(white, &Aabb { min: v3(0.9, 0.0, 1.0), max: v3(2.4, 3.3, 2.5) });
    let short = Mesh::cuboid(white, &Aabb { min: v3(0.0, 0.0, 0.0), max: v3(1.5, 1.65, 1.5) });
    let meshes = vec![room, left, right, tall, short];
    let shift = v3(3.3 - step as f64 * 0.1, 0.0, 2.6);
    let mut instances: Vec<Instance> = (0..4).map(|m| Instance { mesh: m, offset: V3::default() }).collect();
    instances.insert(1, Instance { mesh: 4, offset: shift }); // instance 1 = boîte mobile
    let lights = vec![Light { center: v3(2.75, s - 0.01, 2.75), hx: 0.65, hz: 0.55, emission: V3::splat(15.0), on: light_on }];
    TriScene { meshes, instances, lights, bounds: Aabb { min: V3::splat(-0.1), max: V3::splat(s + 0.1) } }
}

/// Structures d'accélération GPU d'une scène : une BLAS par maillage, une TLAS d'instances.
pub struct Accel {
    pub tlas: wgpu::TlasPackage,
    pub blas: Vec<wgpu::Blas>,
    /// sommets (vec4 alignés) et indices de tous les maillages, pour les normales dans le shader
    pub verts: wgpu::Buffer,
    pub idx: wgpu::Buffer,
    /// par maillage : décalage des sommets, des indices, albédo
    pub mesh_table: wgpu::Buffer,
    vbufs: Vec<wgpu::Buffer>,
    ibufs: Vec<wgpu::Buffer>,
    sizes: Vec<wgpu::BlasTriangleGeometrySizeDescriptor>,
}

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct GMesh {
    pub first_vertex: u32,
    pub first_index: u32,
    pub pad: [u32; 2],
    pub albedo: [f32; 3],
    pub pad2: f32,
}

fn transform(o: V3) -> [f32; 12] {
    [1.0, 0.0, 0.0, o.x as f32, 0.0, 1.0, 0.0, o.y as f32, 0.0, 0.0, 1.0, o.z as f32]
}

impl Accel {
    pub fn new(gpu: &Gpu, scene: &TriScene) -> Accel {
        use wgpu::util::DeviceExt;
        let dev = &gpu.device;
        let mut vbufs = Vec::new();
        let mut ibufs = Vec::new();
        let mut sizes = Vec::new();
        let mut blas = Vec::new();
        let (mut all_v, mut all_i, mut table) = (Vec::<[f32; 4]>::new(), Vec::<u32>::new(), Vec::new());
        for m in &scene.meshes {
            table.push(GMesh {
                first_vertex: all_v.len() as u32,
                first_index: all_i.len() as u32,
                pad: [0; 2],
                albedo: m.albedo,
                pad2: 0.0,
            });
            all_v.extend(m.pos.iter().map(|p| [p[0], p[1], p[2], 0.0]));
            all_i.extend_from_slice(&m.idx);
            vbufs.push(dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("blas v"),
                contents: bytemuck::cast_slice(&m.pos),
                usage: wgpu::BufferUsages::BLAS_INPUT,
            }));
            ibufs.push(dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("blas i"),
                contents: bytemuck::cast_slice(&m.idx),
                usage: wgpu::BufferUsages::BLAS_INPUT,
            }));
            let size = wgpu::BlasTriangleGeometrySizeDescriptor {
                vertex_format: wgpu::VertexFormat::Float32x3,
                vertex_count: m.pos.len() as u32,
                index_format: Some(wgpu::IndexFormat::Uint32),
                index_count: Some(m.idx.len() as u32),
                flags: wgpu::AccelerationStructureGeometryFlags::OPAQUE,
            };
            blas.push(dev.create_blas(
                &wgpu::CreateBlasDescriptor {
                    label: Some("blas"),
                    flags: wgpu::AccelerationStructureFlags::PREFER_FAST_TRACE,
                    update_mode: wgpu::AccelerationStructureUpdateMode::Build,
                },
                wgpu::BlasGeometrySizeDescriptors::Triangles { descriptors: vec![size.clone()] },
            ));
            sizes.push(size);
        }
        let tlas = dev.create_tlas(&wgpu::CreateTlasDescriptor {
            label: Some("tlas"),
            max_instances: scene.instances.len() as u32,
            flags: wgpu::AccelerationStructureFlags::PREFER_FAST_TRACE,
            update_mode: wgpu::AccelerationStructureUpdateMode::Build,
        });
        let st = wgpu::BufferUsages::STORAGE;
        let mut a = Accel {
            tlas: wgpu::TlasPackage::new(tlas),
            verts: dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("verts"),
                contents: bytemuck::cast_slice(&all_v),
                usage: st,
            }),
            idx: dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("idx"),
                contents: bytemuck::cast_slice(&all_i),
                usage: st,
            }),
            mesh_table: dev.create_buffer_init(&wgpu::util::BufferInitDescriptor {
                label: Some("meshes"),
                contents: bytemuck::cast_slice(&table),
                usage: st,
            }),
            blas,
            vbufs,
            ibufs,
            sizes,
        };
        a.build(gpu, scene, true);
        a
    }

    /// (Re)construit la TLAS avec les positions courantes des instances ; les BLAS au premier appel.
    pub fn build(&mut self, gpu: &Gpu, scene: &TriScene, with_blas: bool) {
        for (i, inst) in scene.instances.iter().enumerate() {
            self.tlas[i] = Some(wgpu::TlasInstance::new(&self.blas[inst.mesh], transform(inst.offset), inst.mesh as u32, 0xff));
        }
        let entries: Vec<wgpu::BlasBuildEntry> = if with_blas {
            (0..self.blas.len())
                .map(|m| wgpu::BlasBuildEntry {
                    blas: &self.blas[m],
                    geometry: wgpu::BlasGeometries::TriangleGeometries(vec![wgpu::BlasTriangleGeometry {
                        size: &self.sizes[m],
                        vertex_buffer: &self.vbufs[m],
                        first_vertex: 0,
                        vertex_stride: 12,
                        index_buffer: Some(&self.ibufs[m]),
                        first_index: Some(0),
                        transform_buffer: None,
                        transform_buffer_offset: None,
                    }]),
                })
                .collect()
        } else {
            Vec::new()
        };
        let mut enc = gpu.device.create_command_encoder(&wgpu::CommandEncoderDescriptor { label: Some("as build") });
        enc.build_acceleration_structures(entries.iter(), std::iter::once(&self.tlas));
        gpu.queue.submit(Some(enc.finish()));
    }
}

/// `krc rtsmoke` : normales d'une Cornell box vues par ray query matériel (validation de l'API).
pub fn smoke(gpu: &Gpu) {
    use wgpu::util::DeviceExt;
    let scene = cornell(0, true);
    let accel = Accel::new(gpu, &scene);
    let (w, h) = (512u32, 512u32);
    let dev = &gpu.device;
    let module = dev.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("rtsmoke"),
        source: wgpu::ShaderSource::Wgsl(
            r#"
struct GMesh { first_vertex: u32, first_index: u32, p0: u32, p1: u32, albedo: vec3<f32>, p2: f32 };
@group(0) @binding(0) var acc: acceleration_structure;
@group(0) @binding(1) var<storage, read> verts: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> idx: array<u32>;
@group(0) @binding(3) var<storage, read> meshes: array<GMesh>;
@group(0) @binding(4) var<storage, read_write> img: array<vec4<f32>>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= 512u || gid.y >= 512u) { return; }
    let uv = (vec2<f32>(gid.xy) + 0.5) / 512.0 * 2.0 - 1.0;
    let o = vec3<f32>(2.75, 2.75, 13.0);
    let d = normalize(vec3<f32>(uv.x * 0.36, -uv.y * 0.36, -1.0));
    var rq: ray_query;
    rayQueryInitialize(&rq, acc, RayDesc(0u, 0xffu, 0.001, 1e30, o, d));
    rayQueryProceed(&rq);
    let hit = rayQueryGetCommittedIntersection(&rq);
    var c = vec3<f32>(0.0);
    if (hit.kind != RAY_QUERY_INTERSECTION_NONE) {
        let m = meshes[hit.instance_custom_index];
        let t = m.first_index + 3u * hit.primitive_index;
        let a = verts[m.first_vertex + idx[t]].xyz;
        let b = verts[m.first_vertex + idx[t + 1u]].xyz;
        let e = verts[m.first_vertex + idx[t + 2u]].xyz;
        var n = normalize((hit.object_to_world * vec4<f32>(cross(b - a, e - a), 0.0)).xyz);
        if (dot(n, d) > 0.0) { n = -n; }
        c = m.albedo * (0.35 + 0.65 * max(dot(n, normalize(vec3<f32>(0.3, 0.8, 0.5))), 0.0));
    }
    img[gid.y * 512u + gid.x] = vec4<f32>(c, 1.0);
}
"#
            .into(),
        ),
    });
    let pipe = dev.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some("rtsmoke"),
        layout: None,
        module: &module,
        entry_point: Some("main"),
        compilation_options: Default::default(),
        cache: None,
    });
    let img = dev.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: (w * h * 16) as u64,
        usage: wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
        mapped_at_creation: false,
    });
    let read = dev.create_buffer(&wgpu::BufferDescriptor {
        label: None,
        size: (w * h * 16) as u64,
        usage: wgpu::BufferUsages::MAP_READ | wgpu::BufferUsages::COPY_DST,
        mapped_at_creation: false,
    });
    let bind = dev.create_bind_group(&wgpu::BindGroupDescriptor {
        label: None,
        layout: &pipe.get_bind_group_layout(0),
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: accel.tlas.as_binding() },
            wgpu::BindGroupEntry { binding: 1, resource: accel.verts.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 2, resource: accel.idx.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 3, resource: accel.mesh_table.as_entire_binding() },
            wgpu::BindGroupEntry { binding: 4, resource: img.as_entire_binding() },
        ],
    });
    let _ = dev.create_buffer_init(&wgpu::util::BufferInitDescriptor { label: None, contents: &[0u8; 4], usage: wgpu::BufferUsages::STORAGE });
    let mut enc = dev.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&Default::default());
        pass.set_pipeline(&pipe);
        pass.set_bind_group(0, &bind, &[]);
        pass.dispatch_workgroups(w / 8, h / 8, 1);
    }
    enc.copy_buffer_to_buffer(&img, 0, &read, 0, (w * h * 16) as u64);
    gpu.queue.submit(Some(enc.finish()));
    read.slice(..).map_async(wgpu::MapMode::Read, |r| r.unwrap());
    dev.poll(wgpu::Maintain::Wait);
    let data: Vec<f32> = bytemuck::cast_slice(&read.slice(..).get_mapped_range()).to_vec();
    let px: Vec<V3> = (0..(w * h) as usize).map(|k| v3(data[4 * k] as f64, data[4 * k + 1] as f64, data[4 * k + 2] as f64)).collect();
    std::fs::create_dir_all("results/rt").unwrap();
    crate::image::save("results/rt/smoke_cornell.png", w as usize, h as usize, &px);
    println!("ray query OK : {} triangles, image results/rt/smoke_cornell.png", scene.triangles());
}
