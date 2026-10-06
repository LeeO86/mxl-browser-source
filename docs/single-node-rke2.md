# Single-node RKE2 for testing (SPEC §15.3)

A **test and debug** setup on one Ubuntu 24.04 machine, not a platform mode: the example manifests
in `deploy/` on a one-node RKE2 cluster, with the same pieces the platform uses for GPUs (driver,
container toolkit, device plugin, `RuntimeClass nvidia`; no GPU Operator). Versions follow the
platform's `versions.yml`. Not yet run end to end on a lab machine.

## 1. MXL tmpfs

```sh
sudo mkdir -p /Volumes/mxl
echo 'tmpfs /Volumes/mxl tmpfs defaults,size=8g,mode=1777 0 0' | sudo tee -a /etc/fstab
sudo mount /Volumes/mxl
```

The pod mounts only its own domain directory (`/Volumes/mxl/browser-source-1`, created by the
kubelet as `DirectoryOrCreate`), which lies on this tmpfs.

## 2. RKE2

```sh
curl -sfL https://get.rke2.io | sudo INSTALL_RKE2_VERSION=v1.36.4+rke2r1 sh -
sudo systemctl enable --now rke2-server
export KUBECONFIG=/etc/rancher/rke2/rke2.yaml PATH=$PATH:/var/lib/rancher/rke2/bin
kubectl get nodes
```

A single server takes workloads. If it was installed with a `CriticalAddonsOnly` taint, remove it:
`kubectl taint nodes --all CriticalAddonsOnly-`.

## 3. GPU (optional, for `deploy/mxl-browser-source-gpu.yaml`)

1. The NVIDIA driver on the host (`nvidia-smi` works) and the NVIDIA container toolkit
   (`nvidia-container-toolkit`). Restart `rke2-server` afterwards: RKE2 finds the toolkit and adds
   the containerd runtime handler `nvidia`.
2. The RuntimeClass:

   ```sh
   kubectl apply -f - <<'EOF'
   apiVersion: node.k8s.io/v1
   kind: RuntimeClass
   metadata:
     name: nvidia
   handler: nvidia
   EOF
   ```

3. The device plugin (chart 0.20.1 as on the platform), running with that RuntimeClass:

   ```sh
   helm repo add nvdp https://nvidia.github.io/k8s-device-plugin
   helm install nvidia-device-plugin nvdp/nvidia-device-plugin --version 0.20.1 \
     --namespace kube-system --set runtimeClassName=nvidia
   kubectl get node -o jsonpath='{.items[0].status.allocatable.nvidia\.com/gpu}'
   ```

   Several GPU pods on one GPU need time slicing (the platform's `ts-<n>` configs).

## 4. NMOS registry

The platform's registry image, in namespace `nmos`, where the example manifests look for it
(`nmos-registry.nmos.svc`, registration 3210, query 3211):

```sh
kubectl create namespace nmos
kubectl -n nmos create configmap nmos-registry --from-file=registry.json=docker/registry.json
kubectl -n nmos apply -f - <<'EOF'
apiVersion: apps/v1
kind: Deployment
metadata:
  name: nmos-registry
spec:
  replicas: 1
  selector:
    matchLabels: {app: nmos-registry}
  template:
    metadata:
      labels: {app: nmos-registry}
    spec:
      containers:
        - name: registry
          image: docker.io/gemini2350/nmos-cpp-registry:sha-53481ff@sha256:3c2fa0793ce336e4176487878758efcbd5c61414c5b90433f3758851615f1c91
          env:
            - {name: RUN_MQTT, value: "FALSE"}
            - {name: ADVERTISE_MQTT, value: "FALSE"}
          volumeMounts:
            - {name: config, mountPath: /home/registry.json, subPath: registry.json}
      volumes:
        - name: config
          configMap: {name: nmos-registry}
---
apiVersion: v1
kind: Service
metadata:
  name: nmos-registry
spec:
  selector: {app: nmos-registry}
  ports:
    - {name: registration, port: 3210}
    - {name: query, port: 3211}
EOF
```

## 5. The browser source

```sh
kubectl apply -f deploy/mxl-browser-source.yaml      # or deploy/mxl-browser-source-gpu.yaml
kubectl get pods -w
kubectl port-forward deploy/browser-source-1 8160:8160
```

Open `http://127.0.0.1:8160/`. The ServiceMonitor needs the Prometheus Operator CRDs; without them,
delete that document from the file. The NetworkPolicy assumes the ingress controller in
`kube-system`, the registry in `nmos` and Prometheus in `monitoring`; `kubectl port-forward` is not
affected by it.
